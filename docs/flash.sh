#!/bin/bash
set -euo pipefail

# Flash a BamButton-IDF ESP32-C3 once, then exit.
# Pass the serial port, or use /dev/ttyACM0.
TTY_DEV=${1:-/dev/ttyACM0}

here="$(cd "$(dirname "$0")" && pwd)"
if [[ -f "$here/bootloader.bin" ]]; then
    bootloader="$here/bootloader.bin"
    partition="$here/partition-table.bin"
    otadata="$here/ota_data_initial.bin"
    app="$here/bambutton-idf.bin"
else
    root="$(cd "$here/.." && pwd)"
    bootloader="$root/build/bootloader/bootloader.bin"
    partition="$root/build/partition_table/partition-table.bin"
    otadata="$root/build/ota_data_initial.bin"
    app="$root/build/bambutton-idf.bin"
fi

for image in "$bootloader" "$partition" "$otadata" "$app"; do
    if [[ ! -f "$image" ]]; then
        echo "Missing $image"
        echo "Unpack the release zip and run this script there, or build the project first."
        exit 1
    fi
done

if [[ ! -c "$TTY_DEV" ]]; then
    echo "Port $TTY_DEV is not available."
    exit 1
fi

# Prefer an installed esptool; otherwise run one through uv.
if command -v esptool >/dev/null 2>&1; then
    ESPTOOL=(esptool)
elif command -v uv >/dev/null 2>&1; then
    ESPTOOL=(uvx esptool)
else
    echo "Need uv (https://docs.astral.sh/uv/) or esptool on PATH."
    echo "  uv tool install esptool"
    exit 1
fi

# A full flash replaces the partition table, so the old contents have to go.
"${ESPTOOL[@]}" --chip esp32c3 --port "$TTY_DEV" erase-flash

"${ESPTOOL[@]}" --chip esp32c3 --port "$TTY_DEV" --baud 460800 \
    --before default-reset --after hard-reset write-flash \
    --flash-mode dio --flash-freq 80m --flash-size 4MB \
    0x0 "$bootloader" \
    0x8000 "$partition" \
    0xd000 "$otadata" \
    0x10000 "$app"

echo "Flashed $TTY_DEV."

if ! command -v tio >/dev/null 2>&1; then
    echo "tio is not installed, so the log was not opened."
    exit 0
fi

echo "Opening $TTY_DEV. Quit with Ctrl-T q."
# This board's console is the USB serial/JTAG port. DTR low keeps GPIO9 out
# of download mode. Pulsing RTS resets the chip, the same way the ESP-IDF
# monitor does. The port can vanish during that reset, so reconnect stays
# on, and the pulse runs only the first time the port opens.
tio --baudrate 115200 --flow none \
    --script 'set{DTR=low,RTS=high}; msleep(200); set{DTR=low,RTS=low}' \
    --script-run once \
    "$TTY_DEV"
