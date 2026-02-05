/*
 * mouse-tool [main.c]
 * 
 * Author: Kamil BuriXon Burek
 * Year: 2026
 * Version: 1.0
 * License: GPL v3.0
 * 
 * Description:
 * A terminal-based mouse event capture tool written in C.
 * Detects mouse clicks, motion, and releases, outputs coordinates in CSV or JSON formats,
 * supports multiclick detection, marking clicks, and recording/playback of events.
 */
 
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>
#include <stdint.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <fcntl.h>

#define SGR_BUF 128
#define MAX_EVENTS 65536
#define MULTICLICK_MAX_GAP 0.5
#define MULTICLICK_RADIUS 3

static struct termios orig_tio;
/* default: use stdin fd (original behaviour). May be replaced with /dev/tty if needed. */
static int ttyfd = STDIN_FILENO;
static volatile sig_atomic_t got_sig = 0;
static volatile sig_atomic_t cleanup_done = 0;

typedef enum { EVT_PRESS=1, EVT_MOTION=2, EVT_RELEASE=3 } evtype_t;
typedef struct { int x,y; int button; evtype_t type; struct timespec t; } event_t;
typedef struct { event_t ev; double dt; } out_event_t;

static FILE *out_fp = NULL;
static int do_mark = 0;
static int no_warn = 0;

/* output modes */
enum { OUT_CSV = 0, OUT_JSON = 1, OUT_PRETTY = 2, OUT_JSONL = 3 };

/* formatted error/warn */
static void print_error(int code, const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	fprintf(stderr, "\x1b[31m(error %d)\x1b[0m ", code);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}
static void print_warn(const char *fmt, ...)
{
	if (no_warn) return;
	va_list ap; va_start(ap, fmt);
	fprintf(stderr, "\x1b[33m(warning)\x1b[0m ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}

/* helper: write terminal sequences to the terminal fd.
   If ttyfd is still the default STDIN_FILENO, write to STDOUT_FILENO
   (this preserves original behavior when stdout is a terminal). */
static ssize_t term_write(const char *buf, size_t len)
{
	int fd_to_use = (ttyfd == STDIN_FILENO) ? STDOUT_FILENO : ttyfd;
	return write(fd_to_use, buf, len);
}

/* minimal async-signal-safe restore */
static void minimal_signal_restore(void)
{
	const char seq[] = "\x1b[?25h\x1b[?1049l\x1b[?1000l\x1b[?1002l\x1b[?1006l";
	term_write(seq, sizeof(seq)-1);
}

/* signal handlers */
static void sighandler(int sig) { got_sig = sig; minimal_signal_restore(); }
static void sigwinch_handler(int sig)
{
	(void)sig;
	if (!no_warn) {
		const char msg[] = "\x1b[33mTerminal size changed\x1b[0m\n";
		write(STDERR_FILENO, msg, sizeof(msg)-1);
	}
}

/* full restore */
static void restore_terminal(void)
{
	if (cleanup_done) return;
	cleanup_done = 1;
	/* use term_write for proper fd */
	term_write("\x1b[?1000l\x1b[?1002l\x1b[?1006l", 24);
	/* flush and restore attributes on ttyfd (if valid) */
	if (ttyfd >= 0) tcflush(ttyfd, TCIFLUSH);
	if (ttyfd >= 0) tcsetattr(ttyfd, TCSANOW, &orig_tio);
	term_write("\x1b[?1049l", 8);
	fflush(stdout);
	if (out_fp && out_fp != stdout) fclose(out_fp);
	/* close /dev/tty only if we opened it (i.e., ttyfd != STDIN_FILENO) */
	if (ttyfd != STDIN_FILENO && ttyfd >= 0) { close(ttyfd); ttyfd = STDIN_FILENO; }
}

/* install signals */
static void install_signals(void)
{
	struct sigaction sa; memset(&sa,0,sizeof(sa));
	sa.sa_handler = sighandler; sigemptyset(&sa.sa_mask); sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL); sigaction(SIGQUIT, &sa, NULL);
	struct sigaction sw; memset(&sw,0,sizeof(sw));
	sw.sa_handler = sigwinch_handler; sigemptyset(&sw.sa_mask); sw.sa_flags = SA_RESTART;
	sigaction(SIGWINCH, &sw, NULL);
}

/* enable mouse reporting */
static void enable_mouse_reporting(int motion)
{
	if (motion) term_write("\x1b[?1000h\x1b[?1002h\x1b[?1006h", 24);
	else term_write("\x1b[?1000h\x1b[?1006h", 16);
	/* drain the chosen fd so sequences are sent */
	if (ttyfd == STDIN_FILENO) tcdrain(STDOUT_FILENO); else tcdrain(ttyfd);
}

/* parse SGR "<Cb;Cx;CyM" */
static int parse_sgr(const char *buf, size_t len, int *cb, int *cx, int *cy, char *termch)
{
	if (len < 4 || buf[0] != '<' || len >= SGR_BUF) return 0;
	char tmp[SGR_BUF];
	memcpy(tmp, buf+1, len-2);
	tmp[len-2] = '\0';
	char *p = tmp;
	char *s1 = strchr(p,';'); if (!s1) return 0; *s1++ = '\0';
	char *s2 = strchr(s1,';'); if (!s2) return 0; *s2++ = '\0';
	errno = 0;
	char *end;
	long vcb = strtol(p,&end,10); if (errno || end==p) return 0;
	long vcx = strtol(s1,&end,10); if (errno || end==s1) return 0;
	long vcy = strtol(s2,&end,10); if (errno || end==s2) return 0;
	*cb = (int)vcb; *cx = (int)vcx; *cy = (int)vcy; *termch = buf[len-1];
	return 1;
}

/* read SGR event; return codes:
   1 -> event
   0 -> timeout
  -1 -> EOF/error (or signal set)
   2 -> Enter pressed
*/
static int read_sgr_event_timeout(event_t *ev, double timeout_sec, int want_motion)
{
	fd_set rfds; struct timeval tv;
	time_t sec = (time_t)timeout_sec;
	suseconds_t usec = (suseconds_t)((timeout_sec - (double)sec) * 1e6);
	int rv;
	for (;;) {
		FD_ZERO(&rfds); FD_SET(ttyfd, &rfds);
		if (timeout_sec < 0) rv = select(ttyfd+1, &rfds, NULL, NULL, NULL);
		else { tv.tv_sec = sec; tv.tv_usec = usec; rv = select(ttyfd+1, &rfds, NULL, NULL, &tv); }
		if (rv == -1) {
			if (errno == EINTR) { if (got_sig) return -1; continue; }
			return -1;
		}
		if (rv == 0) return 0;
		char c; ssize_t r = read(ttyfd, &c, 1);
		if (r <= 0) return -1;
		if (c == '\r' || c == '\n') return 2;
		if ((unsigned char)c != 0x1b) continue;
		if (read(ttyfd, &c, 1) <= 0) return -1; if (c != '[') continue;
		if (read(ttyfd, &c, 1) <= 0) return -1; if (c != '<') continue;
		char buf[SGR_BUF]; size_t len = 0; buf[len++] = '<';
		while (len + 1 < sizeof(buf)) {
			if (read(ttyfd, &c, 1) <= 0) return -1;
			buf[len++] = c;
			if (c == 'M' || c == 'm') break;
		}
		int cb,x,y; char termch = 0;
		if (!parse_sgr(buf, len, &cb, &x, &y, &termch)) continue;
		clock_gettime(CLOCK_MONOTONIC, &ev->t);
		ev->button = cb; ev->x = x; ev->y = y;
		if (termch == 'M') { if (cb < 32) ev->type = EVT_PRESS; else ev->type = EVT_MOTION; }
		else ev->type = EVT_RELEASE;
		return 1;
	}
}

/* draw blue dot */
static void draw_mark(int x, int y)
{
	char seq[128];
	int n = snprintf(seq, sizeof(seq),
		"\x1b""7" "\x1b[%d;%dH" "\x1b[34m" "\u25CF" "\x1b[0m" "\x1b""8", y, x);
	if (n>0) term_write(seq, (size_t)n);
	/* flush to terminal fd */
	if (ttyfd == STDIN_FILENO) tcdrain(STDOUT_FILENO); else tcdrain(ttyfd);
}

/* color grad */
static void color_gradient_idx(size_t i, size_t n, int *r, int *g, int *b)
{
	if (n<=1) { *r=255; *g=0; *b=0; return; }
	double t = (double)i / (double)(n-1);
	*r = (int)((1.0 - t)*255.0 + 0.5);
	*g = (int)(t*255.0 + 0.5);
	*b = 0;
	if (*r<0) *r=0; if (*r>255) *r=255;
	if (*g<0) *g=0; if (*g>255) *g=255;
}

/* playback on alt buffer */
static void playback_events_color(event_t *events, size_t n)
{
	if (n == 0) return;
	term_write("\x1b[?1049h", 8);
	term_write("\x1b[?25l", 6);
	if (ttyfd == STDIN_FILENO) tcdrain(STDOUT_FILENO); else tcdrain(ttyfd);
	term_write("\x1b[2J", 4);
	if (ttyfd == STDIN_FILENO) tcdrain(STDOUT_FILENO); else tcdrain(ttyfd);

	for (size_t i = 0; i < n && !got_sig; ++i) {
		if (i>0) {
			double dt = (events[i].t.tv_sec + events[i].t.tv_nsec*1e-9) -
						(events[i-1].t.tv_sec + events[i-1].t.tv_nsec*1e-9);
			if (dt > 0) {
				if (dt > 0.5) dt = 0.5;
				struct timespec ts = { .tv_sec = (time_t)dt, .tv_nsec = (long)((dt - (time_t)dt)*1e9) };
				nanosleep(&ts, NULL);
				if (got_sig) break;
			}
		}
		int R,G,B; color_gradient_idx(i, n, &R,&G,&B);
		char seq[256];
		int row = events[i].y, col = events[i].x;
		if (row<1) row=1; if (col<1) col=1;
		int len = snprintf(seq, sizeof(seq), "\x1b[%d;%dH\x1b[38;2;%d;%d;%dm\u25CF\x1b[0m", row, col, R, G, B);
		if (len>0) term_write(seq, (size_t)len);
		if (ttyfd == STDIN_FILENO) tcdrain(STDOUT_FILENO); else tcdrain(ttyfd);
	}
	if (!got_sig) { struct timespec tpa = { .tv_sec = 1, .tv_nsec = 0 }; nanosleep(&tpa, NULL); }
	term_write("\x1b[?25h", 6);
	term_write("\x1b[?1049l", 8);
	if (ttyfd == STDIN_FILENO) tcdrain(STDOUT_FILENO); else tcdrain(ttyfd);
}

/* helpers */
static int parse_positive_int(const char *s, long *out) {
	if (!s) return 0; errno = 0; char *end; long v = strtol(s,&end,10);
	if (errno || end==s || *end!='\0' || v<=0) return 0; *out = v; return 1;
}
static int parse_positive_double(const char *s, double *out) {
	if (!s) return 0; errno = 0; char *end; double v = strtod(s,&end);
	if (errno || end==s || *end!='\0' || v<=0.0) return 0; *out = v; return 1;
}
static const char *type_str(evtype_t t) { if (t == EVT_PRESS) return "press"; if (t==EVT_RELEASE) return "release"; return "motion"; }

/* print JSON history with metadata
   Note: we count only press events for the "outputs" top-level field. */
static void print_json_history(out_event_t *outs, size_t n, FILE *fp, int pretty, const char *mode, const char *started_at, double duration)
{
	size_t press_count = 0;
	for (size_t i = 0; i < n; ++i) if (outs[i].ev.type == EVT_PRESS) ++press_count;

	if (!fp) fp = stdout;
	if (!pretty) {
		fprintf(fp, "{\"mode\":\"%s\",\"started_at\":\"%s\",\"duration\":%.6f,\"outputs\":%zu,\"events\":[", mode, started_at, duration, press_count);
		for (size_t i=0;i<n;++i) {
			event_t *e = &outs[i].ev;
			fprintf(fp, "%s{\"x\":%d,\"y\":%d,\"button\":%d,\"type\":\"%s\",\"dt\":%.6f}", (i==0)?"":",", e->x,e->y,e->button,type_str(e->type),outs[i].dt);
		}
		fprintf(fp, "]}\n");
	} else {
		fprintf(fp, "{\n  \"mode\": \"%s\",\n  \"started_at\": \"%s\",\n  \"duration\": %.6f,\n  \"outputs\": %zu,\n  \"events\": [\n", mode, started_at, duration, press_count);
		for (size_t i=0;i<n;++i) {
			event_t *e = &outs[i].ev;
			fprintf(fp, "    {\"x\":%d, \"y\":%d, \"button\":%d, \"type\":\"%s\", \"dt\":%.6f}%s\n",
				e->x, e->y, e->button, type_str(e->type), outs[i].dt, (i+1<n)?",":"");
		}
		fprintf(fp, "  ]\n}\n");
	}
	fflush(fp);
}

/* print JSON from events (for record) */
static void print_json_from_events(event_t *events, size_t n, FILE *fp, int pretty, const char *mode, const char *started_at, double duration)
{
	size_t press_count = 0;
	for (size_t i=0;i<n;++i) if (events[i].type == EVT_PRESS) ++press_count;

	if (!fp) fp = stdout;
	if (!pretty) {
		fprintf(fp, "{\"mode\":\"%s\",\"started_at\":\"%s\",\"duration\":%.6f,\"outputs\":%zu,\"events\":[", mode, started_at, duration, press_count);
		for (size_t i=0;i<n;++i) {
			event_t *e = &events[i];
			double dt = 0.0;
			if (i>0) dt = (e->t.tv_sec + e->t.tv_nsec*1e-9) - (events[i-1].t.tv_sec + events[i-1].t.tv_nsec*1e-9);
			fprintf(fp, "%s{\"x\":%d,\"y\":%d,\"button\":%d,\"type\":\"%s\",\"dt\":%.6f}", (i==0)?"":",", e->x,e->y,e->button,type_str(e->type), dt);
		}
		fprintf(fp, "]}\n");
	} else {
		fprintf(fp, "{\n  \"mode\": \"%s\",\n  \"started_at\": \"%s\",\n  \"duration\": %.6f,\n  \"outputs\": %zu,\n  \"events\": [\n", mode, started_at, duration, press_count);
		for (size_t i=0;i<n;++i) {
			event_t *e = &events[i];
			double dt = 0.0;
			if (i>0) dt = (e->t.tv_sec + e->t.tv_nsec*1e-9) - (events[i-1].t.tv_sec + events[i-1].t.tv_nsec*1e-9);
			fprintf(fp, "    {\"x\":%d, \"y\":%d, \"button\":%d, \"type\":\"%s\", \"dt\":%.6f}%s\n",
				e->x, e->y, e->button, type_str(e->type), dt, (i+1<n)?",":"");
		}
		fprintf(fp, "  ]\n}\n");
	}
	fflush(fp);
}

/* print single json line (jsonl) */
static void print_json_line(event_t *e, double dt, FILE *fp)
{
	if (!fp) fp = stdout;
	fprintf(fp, "{\"x\":%d,\"y\":%d,\"button\":%d,\"type\":\"%s\",\"dt\":%.6f}\n", e->x,e->y,e->button,type_str(e->type),dt);
	fflush(fp);
}

/* wait for first press (blocking). returns:
   1 -> got press (ev filled)
   0 -> failure/timeout/enter/signal
*/
static int wait_for_first_press(event_t *ev)
{
	while (!got_sig) {
		int r = read_sgr_event_timeout(ev, -1.0, 0);
		if (r == 1 && ev->type == EVT_PRESS) return 1;
		if (r == 2) return 0; /* Enter pressed -> treat as failure/abort */
		if (r == -1) return 0;
	}
	return 0;
}

/* wait for N-1 more presses near (firstx,firsty). returns 0 success, 1 mismatch/timeout */
static int wait_multiclick_followups(int firstx, int firsty, int total_N)
{
	if (total_N <= 1) return 0;
	int count = 1;
	event_t ev;
	while (count < total_N && !got_sig) {
		int r = read_sgr_event_timeout(&ev, MULTICLICK_MAX_GAP, 0);
		if (r == 0) return 1; /* timeout */
		if (r == -1) return 1;
		if (r == 2) return 1; /* Enter -> treat as failure for multiclick */
		if (ev.type != EVT_PRESS) continue;
		int dx = firstx - ev.x, dy = firsty - ev.y;
		if (dx*dx + dy*dy <= MULTICLICK_RADIUS * MULTICLICK_RADIUS) {
			count++;
			continue;
		} else return 1;
	}
	return (count == total_N) ? 0 : 1;
}

/* handle -c: print LAST click (not first) in selected format; if timeout/mismatch -> print none */
static int handle_click_mode(int N, int out_mode_local, FILE *outfp, int do_mark_local, const char *started_at)
{
	event_t first;
	if (!wait_for_first_press(&first)) {
		/* no first press (enter/signal/EOF) */
		return 1;
	}

	/* accumulate N clicks that must be near the first; update 'last' to the most recent valid press */
	event_t last = first;
	if (N > 1) {
		int count = 1;
		event_t ev;
		while (count < N && !got_sig) {
			int r = read_sgr_event_timeout(&ev, MULTICLICK_MAX_GAP, 0);
			if (r == 0) { /* timeout -> failure */
				return 1;
			}
			if (r == -1) return 1;
			if (r == 2) return 1; /* Enter pressed -> failure */
			if (ev.type != EVT_PRESS) continue;
			int dx = first.x - ev.x, dy = first.y - ev.y;
			if (dx*dx + dy*dy <= MULTICLICK_RADIUS * MULTICLICK_RADIUS) {
				count++;
				last = ev;
				continue;
			} else {
				/* too far -> failure */
				return 1;
			}
		}
		if (count != N) return 1;
	}

	/* Success: emit the last click (not the first) */
	if (do_mark_local) draw_mark(last.x, last.y);

	if (out_mode_local == OUT_JSONL) {
		print_json_line(&last, 0.0, outfp?outfp:stdout);
	} else if (out_mode_local == OUT_JSON || out_mode_local == OUT_PRETTY) {
		out_event_t *tmp = calloc(1, sizeof(*tmp));
		if (!tmp) {
			if (!outfp) outfp = stdout;
			fprintf(outfp, "%d,%d,%d\n", last.x, last.y, last.button);
			fflush(outfp);
		} else {
			tmp[0].ev = last; tmp[0].dt = 0.0;
			print_json_history(tmp, 1, outfp?outfp:stdout, out_mode_local==OUT_PRETTY, "click", started_at, 0.0);
			free(tmp);
		}
	} else {
		if (!outfp) outfp = stdout;
		fprintf(outfp, "%d,%d,%d\n", last.x, last.y, last.button);
		fflush(outfp);
	}

	return 0;
}

/* help */
static void print_help(const char *me)
{
	fprintf(stderr,
"mouse-tool v1.0 (c) Kamil BuriXon Burek 2026\n"
"Capture mouse clicks and movements, retrieve click positions, and record mouse activity directly in the terminal.\n\n"
"Usage:\n"
"  %s [options]\n\n"
"Options:\n"
"  -i, --infinite           keep running, print unique X,Y per change\n"
"  -n, --count N            stop after N outputs (exclusive with --infinite)\n"
"  -c, --click N            detect N clicks at same/near position (<=0.5s gap) and print last click (or none on timeout/mismatch)\n"
"  -m, --mark               draw a dot at click position (works in any mode)\n"
"  -r, --record SEC         record SEC seconds then playback colorized (old->red, new->green)\n"
"  -j, --json               collect history and emit JSON at exit\n"
"  -p, --pretty-json        same as --json but pretty-printed\n"
"  -l, --jsonl              newline-delimited JSON output (streaming)\n"
"  -o, --outfile FILE       append outputs to FILE or create it\n"
"  -a, --append             append to existing outfile (use with -o)\n"
"  -O, --overwrite          overwrite existing outfile (use with -o)\n"
"  -N, --no-warn            suppress warnings\n"
"  -h, --help               show this help\n\n"
"Short options may be combined (e.g. -im or -mn7).\n"
"CSV mode streams lines \"X,Y,button\" (default).\n"
"JSON modes produce JSON metadata + events at exit; --jsonl streams newline-delimited JSON lines.\n"
"Press Enter during continuous/recording to stop listening and finish normally (dump & playback).\n"
"Exit codes: 0 ok, 1 general error / -c failure, 2 invalid parameter, 3 file not writable, 4 file exists.\n",
	me);
}

/* main */
int main(int argc, char **argv)
{
	int infinite = 0;
	int count_limit = 0;
	int click_mode = 0;
	int click_N = 0;
	int record_mode = 0;
	double record_seconds = 0.0;
	int out_mode = OUT_CSV;
	int append_flag = 0;
	int overwrite_flag = 0;
	char *outfile_path = NULL;

	static struct option longopts[] = {
		{"infinite", no_argument, NULL, 'i'},
		{"count", required_argument, NULL, 'n'},
		{"click", required_argument, NULL, 'c'},
		{"mark", no_argument, NULL, 'm'},
		{"record", required_argument, NULL, 'r'},
		{"json", no_argument, NULL, 'j'},
		{"pretty-json", no_argument, NULL, 'p'},
		{"jsonl", no_argument, NULL, 'l'},
		{"outfile", required_argument, NULL, 'o'},
		{"append", no_argument, NULL, 'a'},
		{"overwrite", no_argument, NULL, 'O'},
		{"no-warn", no_argument, NULL, 'N'},
		{"help", no_argument, NULL, 'h'},
		{0,0,0,0}
	};

	int ch;
	while ((ch = getopt_long(argc, argv, "in:c:mr:o:apOlNjh", longopts, NULL)) != -1) {
		if (ch == 'i') infinite = 1;
		else if (ch == 'n') { long v; if (!parse_positive_int(optarg,&v)) { print_error(2,"--count/-n requires positive integer"); return 2; } count_limit = (int)v; }
		else if (ch == 'c') { long v; if (!parse_positive_int(optarg,&v)) { print_error(2,"--click/-c requires positive integer"); return 2; } click_mode = 1; click_N = (int)v; }
		else if (ch == 'm') do_mark = 1;
		else if (ch == 'r') { double v; if (!parse_positive_double(optarg,&v)) { print_error(2,"--record/-r requires positive numeric seconds"); return 2; } record_mode = 1; record_seconds = v; }
		else if (ch == 'j') out_mode = OUT_JSON;
		else if (ch == 'p') out_mode = OUT_PRETTY;
		else if (ch == 'l') out_mode = OUT_JSONL;
		else if (ch == 'o') { if (!optarg) { print_error(2,"--outfile/-o requires a file path"); return 2; } outfile_path = optarg; }
		else if (ch == 'a') append_flag = 1;
		else if (ch == 'O') overwrite_flag = 1;
		else if (ch == 'N') no_warn = 1;
		else if (ch == 'h') { print_help(argv[0]); return 0; }
		else { print_error(2,"unknown parameter"); return 2; }
	}

	/* exclusivity */
	if (infinite && count_limit) { print_error(2,"--infinite and --count are exclusive"); return 2; }
	if (click_mode && (infinite || count_limit || record_mode)) { print_error(2,"--click is exclusive with --infinite/--count/--record"); return 2; }
	if (record_mode && click_mode) { print_error(2,"--record and --click are exclusive"); return 2; }

	/* If stdout or stdin are not ttys, try to open /dev/tty for terminal interactions.
	   This preserves ability to capture mouse from the controlling terminal while
	   allowing stdout to be a pipe (so "$(mouse-tool)" works). */
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		int tfd = open("/dev/tty", O_RDWR | O_NOCTTY);
		if (tfd != -1) {
			ttyfd = tfd;
		} else {
			/* fallback to original behaviour: require interactive terminal */
			print_error(2,"needs interactive terminal");
			return 2;
		}
	}

	if (!isatty(ttyfd)) { print_error(2,"needs interactive terminal"); return 2; }

	/* outfile handling */
	if (append_flag && !outfile_path) { print_warn("append requested but no outfile specified; continuing without append"); append_flag = 0; }
	if (outfile_path) {
		struct stat stbuf;
		int exists = (stat(outfile_path, &stbuf) == 0);
		if (exists) {
			if (!append_flag && !overwrite_flag) { print_error(4,"output file '%s' exists (use -a to append or -O to overwrite)", outfile_path); if (ttyfd != STDIN_FILENO) close(ttyfd); return 4; }
			if (access(outfile_path, W_OK) != 0) { print_error(3,"output file '%s' is not writable", outfile_path); if (ttyfd != STDIN_FILENO) close(ttyfd); return 3; }
		}
		if (append_flag) out_fp = fopen(outfile_path, "a");
		else out_fp = fopen(outfile_path, "w");
		if (!out_fp) { print_error(3,"cannot open output file '%s': %s", outfile_path, strerror(errno)); if (ttyfd != STDIN_FILENO) close(ttyfd); return 3; }
	}

	/* setup tty attributes on ttyfd (which may be STDIN_FILENO or /dev/tty) */
	if (tcgetattr(ttyfd, &orig_tio) == -1) { print_error(1,"tcgetattr failed: %s", strerror(errno)); if (ttyfd != STDIN_FILENO) close(ttyfd); return 1; }
	struct termios tio = orig_tio;
	tio.c_lflag &= ~(ICANON | ECHO);
	tio.c_cc[VMIN] = 1; tio.c_cc[VTIME] = 0;
	if (tcsetattr(ttyfd, TCSANOW, &tio) == -1) { print_error(1,"tcsetattr failed: %s", strerror(errno)); if (ttyfd != STDIN_FILENO) close(ttyfd); return 1; }
	atexit(restore_terminal);
	install_signals();

	/* click mode: wait for first press, print it according to format, then wait for followups */
	if (click_mode) {
		enable_mouse_reporting(0);
		char started_at[64] = ""; { time_t t = time(NULL); struct tm g; gmtime_r(&t,&g); strftime(started_at, sizeof(started_at), "%Y-%m-%dT%H:%M:%SZ", &g); }
		int rc = handle_click_mode(click_N, out_mode, out_fp, do_mark, started_at);
		restore_terminal();
		return rc == 0 ? 0 : 1;
	}

	int want_motion = (infinite || record_mode || count_limit > 0) ? 1 : 0;
	enable_mouse_reporting(want_motion);

	/* allocate events if record */
	event_t *events = NULL;
	size_t ev_count = 0, max_events = 0;
	if (record_mode) {
		size_t est = (size_t)(record_seconds * 1000.0) + 1024;
		if (est > MAX_EVENTS) est = MAX_EVENTS;
		max_events = est;
		events = calloc(max_events, sizeof(*events));
		if (!events) { print_error(1,"cannot allocate events buffer"); return 1; }
	}

	out_event_t *outs = NULL; size_t outs_count = 0, outs_cap = 0;

	event_t ev; event_t last_print = {0}; int have_last_print = 0;
	int outputs = 0; /* counts only PRESS events */
	struct timespec rec_start, now, last_emit_time = {0};

	if (record_mode) clock_gettime(CLOCK_MONOTONIC, &rec_start);

	/* main loop */
	for (;;) {
		if (got_sig) break;
		/* compute timeout */
		double timeout = -1.0;
		if (record_mode) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			double elapsed = (now.tv_sec - rec_start.tv_sec) + (now.tv_nsec - rec_start.tv_nsec) * 1e-9;
			double remaining = record_seconds - elapsed;
			if (remaining <= 0.0) break;
			timeout = remaining;
		}

		int rv = read_sgr_event_timeout(&ev, timeout, want_motion);
		if (rv == -1) break;
		if (rv == 0) {
			/* timeout */
			if (record_mode) continue;
			else continue;
		}
		if (rv == 2) { /* Enter pressed */
			break;
		}

		/* record mode: just store */
		if (record_mode) {
			if (ev_count < max_events) events[ev_count++] = ev;
			continue;
		}

		/* IMPORTANT: if we're not in motion mode, we should ignore motion and release
		   for immediate-emission channels (CSV and also for counting). However JSON modes
		   may still want to record all events: we'll still *store* all events for JSON/pretty,
		   but for immediate CSV emission and for counting we only consider PRESS. */

		/* mark if requested only for presses */
		if (do_mark && ev.type == EVT_PRESS) draw_mark(ev.x, ev.y);

		/* compute dt relative to last_emit_time for JSONL or outs */
		double dt = 0.0;
		struct timespec cur; clock_gettime(CLOCK_MONOTONIC, &cur);
		if (last_emit_time.tv_sec != 0 || last_emit_time.tv_nsec != 0) {
			dt = (cur.tv_sec - last_emit_time.tv_sec) + (cur.tv_nsec - last_emit_time.tv_nsec) * 1e-9;
		}
		last_emit_time = cur;

		/* JSONL: stream every event (press/release/motion) as they come */
		if (out_mode == OUT_JSONL) {
			print_json_line(&ev, dt, out_fp?out_fp:stdout);
		} else if (out_mode == OUT_JSON || out_mode == OUT_PRETTY) {
			/* store every event for final JSON dump (so JSON will show press+release/motions) */
			if (outs_count + 1 > outs_cap) {
				size_t newcap = outs_cap ? outs_cap * 2 : 256;
				out_event_t *tmp = realloc(outs, newcap * sizeof(*tmp));
				if (!tmp) {
					print_error(1, "out of memory");
					break;
				}
				outs = tmp; outs_cap = newcap;
			}
			outs[outs_count].ev = ev;
			outs[outs_count].dt = dt;
			outs_count++;
		} else { /* CSV mode: only emit PRESS events (X,Y,button) once per press */
			if (ev.type == EVT_PRESS) {
				FILE *fp = out_fp ? out_fp : stdout;
				fprintf(fp, "%d,%d,%d\n", ev.x, ev.y, ev.button);
				fflush(fp);
			} else {
				/* ignore release/motion for CSV */
			}
		}

		/* increment outputs only for press events (so -n and default behavior count presses) */
		if (ev.type == EVT_PRESS) outputs++;

		/* remember last_print if needed (kept for compatibility) */
		last_print = ev; have_last_print = 1;

		/* Default behavior (no -i and no -n): report one press and exit.
		   And -n counts only presses. */
		if (!infinite && count_limit == 0) {
			if (outputs >= 1) break;
		}

		if (!infinite && count_limit > 0 && outputs >= count_limit) break;
	}

	/* finished main loop */
	/* restore terminal at end (will also close /dev/tty if we opened it) after handling outputs */
	if (record_mode) {
		/* playback and dump events */
		char started_at[64] = ""; { time_t t = time(NULL); struct tm g; gmtime_r(&t,&g); strftime(started_at, sizeof(started_at), "%Y-%m-%dT%H:%M:%SZ", &g); }
		/* compute duration */
		double duration = 0.0;
		if (ev_count > 1) {
			duration = (events[ev_count-1].t.tv_sec - events[0].t.tv_sec) + (events[ev_count-1].t.tv_nsec - events[0].t.tv_nsec)*1e-9;
		}
		restore_terminal();
		playback_events_color(events, ev_count);
		if (out_mode == OUT_JSONL) {
			for (size_t i=0;i<ev_count;++i) {
				double dt = 0.0;
				if (i>0) dt = (events[i].t.tv_sec + events[i].t.tv_nsec*1e-9) - (events[i-1].t.tv_sec + events[i-1].t.tv_nsec*1e-9);
				print_json_line(&events[i], dt, out_fp?out_fp:stdout);
			}
		} else if (out_mode == OUT_JSON || out_mode == OUT_PRETTY) {
			print_json_from_events(events, ev_count, out_fp?out_fp:stdout, out_mode==OUT_PRETTY, "record", started_at, duration);
		} else {
			/* CSV */
			FILE *fp = out_fp ? out_fp : stdout;
			for (size_t i=0;i<ev_count;++i) {
				if (events[i].type == EVT_PRESS) fprintf(fp, "%d,%d,%d\n", events[i].x, events[i].y, events[i].button);
			}
			fflush(fp);
		}
		free(events);
	} else {
		/* non-record: if JSON history requested, print stored outs */
		if (out_mode == OUT_JSON || out_mode == OUT_PRETTY) {
			char started_at[64] = ""; { time_t t = time(NULL); struct tm g; gmtime_r(&t,&g); strftime(started_at, sizeof(started_at), "%Y-%m-%dT%H:%M:%SZ", &g); }
			double duration = 0.0;
			if (outs_count > 1) {
				duration = 0.0;
				for (size_t i=0;i<outs_count;++i) duration += outs[i].dt;
			}
			restore_terminal();
			print_json_history(outs, outs_count, out_fp?out_fp:stdout, out_mode==OUT_PRETTY, "stream", started_at, duration);
			free(outs);
		} else {
			restore_terminal();
		}
	}

	return 0;
}
