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
