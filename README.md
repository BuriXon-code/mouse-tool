# 🖱️ mouse-tool - Terminal mouse click & motion tracker tool 🎯

## About
**mouse-tool** is a lightweight and versatile command-line utility written in **C** that captures mouse clicks, motion, and release events directly in your terminal. It supports both Linux and Termux, allowing you to log X,Y coordinates of mouse interactions in real-time without a GUI.

Perfect for terminal demos, automation, recording sequences of clicks, or just experimenting with mouse input in terminal apps. It can output in **CSV**, **JSON**, **pretty JSON**, or **newline-delimited JSON (JSONL)** and even render a visual playback of clicks in color on the terminal screen.

## Features
- Capture terminal mouse clicks, releases, and motion events.
- Multi-click detection with configurable gap and radius.
- JSON, JSONL, pretty JSON, and CSV output formats.
- Optional marking of click positions with colored dots.
- Record sessions with playback in color gradient (old -> red, new -> green).
- Continuous streaming mode or fixed number of clicks/events.
- Works in Termux and Linux terminal emulators supporting SGR mouse mode.
- Robust POSIX signal handling (SIGINT, SIGTERM, SIGHUP, SIGWINCH).
- Minimal dependencies — just a C toolchain, no external libraries.

## Installation

### Install dependencies / Setup environment

**Debian / Ubuntu**
```
sudo apt update
sudo apt install -y build-essential git
```

**Arch / Manjaro**
```
sudo pacman -Syu --needed base-devel git
```

**Fedora / RHEL / CentOS**
```
sudo dnf install -y gcc make git
```

**for older RHEL/CentOS**
```
sudo yum install -y gcc make git
```

**Alpine Linux**
```
sudo apk add build-base git
```

**Termux (Android)**
```
pkg update && pkg upgrade
pkg install git clang make
```

### Clone repository and build
```
git clone https://github.com/BuriXon-code/mouse-tool
cd mouse-tool
```

**Build**
```
clang -O2 main.c -o mouse-tool
chmod +x mouse-tool
```

### Add to the system `$PATH`

**Linux**
```
sudo mv mouse-tool /usr/bin/mouse-tool
```

**Termux**
```
mv mouse-tool /data/data/com.termux/files/usr/bin/mouse-tool
```

## Usage

> [!NOTE]
> mouse-tool requires a TTY for mouse events. Full behavior may not work if stdout or stdin is redirected.

### Options (summary)

| Option | Description |
|--------|-------------|
| `-i, --infinite` | Keep running and print unique X,Y per change. |
| `-n, --count N` | Stop after N outputs (press events). |
| `-c, --click N` | Detect N clicks at the same/near position and print the first click. |
| `-m, --mark` | Draw a dot at click positions. |
| `-r, --record SEC` | Record SEC seconds of events and playback in color. |
| `-j, --json` | Collect history and emit JSON at exit. |
| `-p, --pretty-json` | Same as JSON but pretty-printed. |
| `-l, --jsonl` | Stream newline-delimited JSON lines. |
| `-o, --outfile FILE` | Save output to a file. |
| `-a, --append` | Append to existing outfile. |
| `-O, --overwrite` | Overwrite existing outfile. |
| `-N, --no-warn` | Suppress warnings. |
| `-h, --help` | Show help and exit. |

> [!NOTE]
> The `-c` | `--click` option additionally returns a return code of 0 if N clicks occur within a sufficiently short time at the same position, or 1 if the subsequent click is too far from the first one or too slow.  
> This is intended as an implementation for double-click (multi-click) detection of an element.

> [!NOTE]
> In `-i`, `-n`, `-r`, and other modes, pressing the Enter key triggers a soft stop of the script and parses the data collected so far.

### Usage examples

Simple: capture a single click and print coordinates
```
./mouse-tool
```

Continuous streaming with motion events (JSONL)
```
./mouse-tool -i -l
```

Detect 3 clicks in succession at the same spot
```
./mouse-tool -c 3 -m
```

Record 10 seconds of events and playback in color
```
./mouse-tool -r 10 -p
```

Save output to file
```
./mouse-tool -i -o clicks.jsonl -l
```

### Implementation example

Example implementation in a simple Bash script where using **mouse-tool** allows detecting one of two available options by clicking on it.

```bash
#!/bin/bash
BTN1_ROW=3; BTN1_COL_START=5; BTN1_COL_END=9
BTN2_ROW=3; BTN2_COL_START=15; BTN2_COL_END=19
CLR1="\e[42m"
CLR2="\e[41m"
RESET="\e[0m"
while true; do
    clear
    tput cup $((BTN1_ROW-1)) $BTN1_COL_START
    echo -ne "${CLR1}OPT1${RESET}"
    tput cup $((BTN2_ROW-1)) $BTN2_COL_START
    echo -ne "${CLR2}OPT2${RESET}"
    read -r XY < <(mouse-tool)
    X=$(echo $XY | cut -d',' -f1)
    Y=$(echo $XY | cut -d',' -f2)
    if [[ "$Y" -eq $BTN1_ROW && "$X" -ge $BTN1_COL_START && "$X" -le $BTN1_COL_END ]]; then
        clear
        echo "You clicked OPT1"
        exit 0
    elif [[ "$Y" -eq $BTN2_ROW && "$X" -ge $BTN2_COL_START && "$X" -le $BTN2_COL_END ]]; then
        clear
        echo "You clicked OPT2"
        exit 0
    fi
done
```

> [!NOTE]
> The Bash script has been placed in the repository as `test.sh`.
> Before using `test.sh` you must compile the program and place it in `$PATH`.

## License

**mouse-tool** is released under **GPL v3.0 (GNU General Public License v3.0)**.

### You **can**:
- Use the program for personal, educational, or commercial purposes.
- Modify the source code to suit your needs.
- Share your modified or unmodified version of the program.
- Include it in other GPL-compatible projects.

### You **cannot**:
- Remove the original copyright notice and license.
- Distribute the software under a proprietary license.
- Claim the program as entirely your own work.
- Impose additional restrictions beyond GPLv3 when redistributing.

> [!NOTE]
> GPLv3 ensures freedom to use and modify while keeping the same freedoms for downstream users.

## Support
### Contact me:
For any issues, suggestions, or questions, reach out via:

- *Email:* support@burixon.dev
- *Contact form:* [Click here](https://burixon.dev/contact/)
- *Bug reports:* [Click here](https://burixon.dev/bugreport/#mouse-tool)

### Support me:
If you find this tool useful, consider supporting my work by making a donation:

[**Donations**](https://burixon.dev/donate/)

Your contributions help develop new projects and improve existing tools! 🚀
