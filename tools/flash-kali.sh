#!/bin/bash
# KALI SMASH flasher for macOS. Needs dfu-util (brew install dfu-util) and a USB data cable.
#
# First time (or coming from stock / an older internal-flash build):
#   flash-kali.sh --first KaliSmash-xxx.bin
#   Hold BOOT, press and release RESET, release BOOT. The script installs the Daisy bootloader, then
#   catches the bootloader's 2-second window by itself and installs KALI SMASH. No timing on your side.
#
# Every update after that:
#   On the Kali: SYSTEM -> UPDATE, turn right to GO. Then: flash-kali.sh KaliSmash-xxx.bin
#   (or RESET and the script still catches the 2-second window)
#
# The two modes share a USB ID; they're told apart by the memory map they report:
#   factory ROM DFU   "@Internal Flash  /0x08000000/..."
#   Daisy bootloader  "@Flash /0x90000000/.../0x90040000/..."
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BOOTLOADER="$HERE/dsy_bootloader_v6_4-intdfu-2000ms.bin"
[ -f "$BOOTLOADER" ] || BOOTLOADER="$HERE/../release/dsy_bootloader_v6_4-intdfu-2000ms.bin"

FIRST=0
if [ "${1:-}" = "--first" ]; then
    FIRST=1
    shift
fi
APP="${1:-}"
if [ -z "$APP" ] || [ ! -f "$APP" ]; then
    echo "usage: $0 [--first] KaliSmash-xxx.bin" >&2
    exit 2
fi
command -v dfu-util >/dev/null || { echo "dfu-util not found: brew install dfu-util" >&2; exit 1; }

# flash <address> <file>: dfu-util often reports an error on the final "leave" even when the write
# succeeded, so success is judged by its "Download done" line
flash() {
    local out
    out=$(dfu-util -a 0 -s "$1:leave" -D "$2" -d ,0483:df11 2>&1) || true
    if echo "$out" | grep -q -E "Download done|File downloaded successfully"; then
        echo "     Written."
    else
        echo "$out" | tail -5 >&2
        echo "Write failed." >&2
        exit 1
    fi
}

# wait_for <pattern> <seconds>: poll the DFU device list until the pattern shows up
wait_for() {
    local pat="$1" limit="$2" start
    start=$(date +%s)
    while ! dfu-util -l 2>/dev/null | grep -q "$pat"; do
        if [ $(($(date +%s) - start)) -ge "$limit" ]; then
            return 1
        fi
        sleep 0.05
    done
}

if [ "$FIRST" = 1 ]; then
    [ -f "$BOOTLOADER" ] || { echo "can't find dsy_bootloader_v6_4-intdfu-2000ms.bin" >&2; exit 1; }
    echo "1/2  Hold BOOT, press and release RESET, then release BOOT (factory programming mode)."
    if ! wait_for "Internal Flash" 120; then
        echo "No factory DFU device after 2 minutes. Check the cable and try again." >&2
        exit 1
    fi
    echo "     Found it. Installing the Daisy bootloader (v6.4, 2000 ms)..."
    flash 0x08000000 "$BOOTLOADER"
    echo "2/2  Waiting for the Daisy bootloader to come up..."
    if ! wait_for "0x90040000" 20; then
        echo "The Daisy bootloader didn't show up. Press RESET and run this again without --first." >&2
        exit 1
    fi
else
    echo "Waiting for the Daisy bootloader (SYSTEM -> UPDATE -> GO on the Kali, or press RESET)..."
    if ! wait_for "0x90040000" 120; then
        if dfu-util -l 2>/dev/null | grep -q "Internal Flash"; then
            echo "The Kali is in factory ROM mode (BOOT held during RESET). Use --first, or press RESET alone." >&2
        else
            echo "No Daisy bootloader after 2 minutes." >&2
        fi
        exit 1
    fi
fi

echo "     Found it. Installing $(basename "$APP")..."
flash 0x90040000 "$APP"
echo "Done. KALI SMASH starts by itself (press RESET if the screen stays on UPDATE MODE)."
