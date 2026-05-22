# Cardputer ADV User Demo

User demo source code of [Cardputer ADV](https://docs.m5stack.com/en/products/sku/K132-Adv).

## Quick Start

This firmware targets **M5Stack Cardputer ADV** and uses **ESP-IDF v5.4.2** with target
`esp32s3`.

### Clone The Development Branch

```bash
git clone git@github.com:coverdh/M5Cardputer-UserDemo.git
cd M5Cardputer-UserDemo
git switch codex/home-control
```

### Fetch Dependencies

The repository keeps third-party source dependencies outside Git history. Fetch them before the
first build:

```bash
python3 ./fetch_repos.py
```

### Install ESP-IDF

Install [ESP-IDF v5.4.2](https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32s3/index.html)
and export it before running `idf.py`.

If this repository is inside the shared `ESP-Dev` workspace used during development, export the
checked-out ESP-IDF like this:

```bash
. ../reference/esp-idf-v5.4.2/export.sh
```

If ESP-IDF is installed elsewhere, use that checkout's `export.sh` instead.

### Configure Target

Run this once after a clean checkout or after switching build targets:

```bash
idf.py set-target esp32s3
```

### Build

```bash
idf.py build
```

The generated firmware image is:

```text
build/cardputer-adv.bin
```

### Flash

Find the serial port first:

```bash
ls /dev/cu.*
```

Then flash with the detected port, for example:

```bash
idf.py -p /dev/cu.usbmodem101 flash
```

### Monitor Logs

```bash
idf.py -p /dev/cu.usbmodem101 monitor
```

Exit monitor with `Ctrl+]`.

## Hardware Notes

### External Input

The firmware has a global `ExternalInput` service used by Launcher, Keyboard, and GameBoy.

Supported devices:

- M5 Chain Joy Stick over ChainBus UART.
- M5 Unit ChainBus followed by a GPIO dual button.
- I2C Joystick Unit, Joystick2, and Byte Button style input devices.

The built-in `ExtIO` app is useful for checking whether a 4-pin external input is present and
whether raw data changes while moving the joystick or pressing buttons.

### Chain Joy Stick

The Chain Joy Stick is a ChainBus UART device. The firmware probes both UART pin orders and logs
the detected pins. A healthy startup log looks like:

```text
chain device 1 type=0x0004
chain input detected: count=1 joystick_id=1
external input: bus=ex-uart joystick=chain-joystick
```

### Dual Button

The regular M5Stack Dual Button Unit is a GPIO unit, not a Chain device. It cannot appear directly
behind Chain Joy Stick unless a Unit ChainBus adapter is in the chain.

Supported chain wiring:

```text
Cardputer ADV -> Chain Joy Stick -> Unit ChainBus -> Dual Button
```

When Unit ChainBus is detected, logs should include:

```text
chain device 2 type=0x0006
chainbus dual button enabled
```

Button mapping for GameBoy:

- Red / GPIO1 -> B
- Blue / GPIO2 -> A

### Direction Mapping

Joystick direction mapping is persisted in NVS and applies globally, including GameBoy.

Open `ExtIO` and use:

- `U`: toggle up/down flip.
- `L`: toggle left/right flip.
- `S`: toggle X/Y axis swap.
- `R` or Enter: rescan external input.

The screen shows the current `flip L/R`, `U/D`, and `swap` states.

### GNSS / LoRa 4-Pin Port

The GNSS/LoRa cap uses GPIO13/GPIO15 as GPS UART. Do not treat this port as a normal spare
Dual Button input unless the firmware is explicitly changed for that wiring. If the internal I2C
scan suddenly shows many fake addresses and the keyboard fails to initialize, remove the external
device from that port and reboot.

## Development Workflow

### Useful Checks

```bash
git status --short
idf.py build
```

For device validation:

```bash
idf.py -p /dev/cu.usbmodem101 flash
idf.py -p /dev/cu.usbmodem101 monitor
```

### Commit Style

Commit messages use Chinese after a Conventional Commits type prefix:

```text
feat: 接入外部手柄和按键输入
fix: 修复外设输入掉线
docs: 添加固件开发文档
```

## Acknowledgments

This project references the following open-source libraries and resources:

- https://github.com/adafruit/Adafruit_TCA8418
- https://github.com/m5stack/M5Unified.git
- https://github.com/pikasTech/PikaPython
- https://github.com/jgromes/RadioLib
- https://github.com/raysan5/raylib
- https://github.com/mikalhart/TinyGPSPlus
- https://github.com/m5stack/M5GFX.git
- https://github.com/Forairaaaaa/mooncake_log
- https://github.com/hhuysqt/esp32s3-keyboard
- https://github.com/78/xiaozhi-esp32
- https://github.com/Forairaaaaa/mooncake
- https://github.com/Forairaaaaa/smooth_ui_toolkit
