#!/bin/bash

# Flash a BamButton-IDF ESP32-C3. Pass the serial port, or use /dev/ttyACM0.
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

waiting_logged=false

while true; do
    if [[ ! -c "$TTY_DEV" ]]; then
        if [[ "$waiting_logged" = false ]]; then
            echo "Port $TTY_DEV is not available, waiting for a board..."
            waiting_logged=true
        fi
        sleep 1
        continue
    fi

    waiting_logged=false

    # A full flash replaces the partition table, so the old contents have to go.
    "${ESPTOOL[@]}" --chip esp32c3 --port "$TTY_DEV" erase-flash

    "${ESPTOOL[@]}" --chip esp32c3 --port "$TTY_DEV" --baud 460800 \
        --before default-reset --after hard-reset write-flash \
        --flash-mode dio --flash-freq 80m --flash-size 4MB \
        0x0 "$bootloader" \
        0x8000 "$partition" \
        0xd000 "$otadata" \
        0x10000 "$app"

    if command -v tio >/dev/null 2>&1; then
        sleep 1
        # -n: exit when the port closes, then this loop flashes the next board.
        tio "$TTY_DEV" -n
    else
        echo "Flashed. Unplug the board to flash another, or press Ctrl-C."
        while [[ -c "$TTY_DEV" ]]; do
            sleep 1
        done
    fi
done
