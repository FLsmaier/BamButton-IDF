# Flashing BamButton-IDF

The board is an ESP32-C3 Super Mini with 4 MB of flash. The console is the USB serial/JTAG port, usually `/dev/ttyACM0`. Nothing else should have that port open while flashing.

## Files

The release zip is flat. Every file sits in the same directory:

- `flash.sh`
- `bootloader.bin` at `0x0`
- `partition-table.bin` at `0x8000`
- `ota_data_initial.bin` at `0xd000`
- `bambutton-idf.bin` at `0x10000`, also the file to upload on the device page
- `bambutton-idf-flash.bin`, the same images merged into one file for offset `0x0`

A build in this repository writes those images under `build/`. `./docs/flash.sh` finds them there.

## Partition layout

| Partition | Offset   | Size    |
|-----------|----------|---------|
| nvs       | `0x9000` | 16 KB   |
| otadata   | `0xd000` | 8 KB    |
| phy_init  | `0xf000` | 4 KB    |
| ota_0     | `0x10000` | 1984 KB |
| ota_1     | `0x200000` | 1984 KB |

`flash.sh` erases the chip first, so stored settings are cleared. Uploading `bambutton-idf.bin` on the device page writes only the application and keeps those settings.

## esptool

Install [uv](https://docs.astral.sh/uv/), then install esptool once:

```bash
uv tool install esptool
```

`uvx esptool` runs it without that install. An `esptool` already on `PATH` is used first.

## Flash

```bash
./flash.sh
./flash.sh /dev/ttyACM0
```

The script erases the flash, writes the four images once, and then opens `tio`. Quit that session with Ctrl-T then q, and the script exits.

The same write, run by hand:

```bash
esptool --chip esp32c3 --port /dev/ttyACM0 --baud 460800 \
    --before default-reset --after hard-reset write-flash \
    --flash-mode dio --flash-freq 80m --flash-size 4MB \
    0x0 bootloader.bin \
    0x8000 partition-table.bin \
    0xd000 ota_data_initial.bin \
    0x10000 bambutton-idf.bin
```

Or write the merged image alone:

```bash
esptool --chip esp32c3 --port /dev/ttyACM0 --baud 460800 \
    write-flash --flash-mode dio --flash-freq 80m --flash-size 4MB \
    0x0 bambutton-idf-flash.bin
```

## Ports

- Linux: `/dev/ttyACM0`
- macOS: `/dev/cu.usbmodem*`
- Windows: `COM3` and similar

If the write fails, run it again at `115200` instead of `460800`.
