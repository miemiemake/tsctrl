# pic_probe: ESP32 test tool for the C3 board's PIC

`pic_probe` lets an ESP32 find and talk to the PIC on a ZKTeco C3 / ZEM500-style
carrier board through the J5/J6 headers. The PIC handles the readers, relays and
door inputs. It is controlled over a 3.3 V serial line with ZK's MCU protocol
(EDK500 guide chapter 9, `fp-firmware/trunk/M880trunk/exfun.c`).

It has three modes:

| Mode | Old controller | What the ESP32 does |
| --- | --- | --- |
| `sniff` | Still fitted and running | Only listens on both wires, prints every frame |
| `scan` | Removed | Sends "get version" (harmless) on each candidate link and baud rate, reports which one answers |
| `talk` | Removed | Acts as the controller: unlock, beep, read clock, door state, card events |

## What you need

- Any ESP32 dev board (ESP32-WROOM DevKit is fine).
- Six 1 kΩ resistors, jumper wires.
- Power the ESP32 from USB during tests. Connect **only GND** to the C3 board,
  not 3.3 V, so the two supplies do not fight.

## Wiring

The header is not fully confirmed for C3 boards, so the tool tries three links.
Wire all three at once. The 1 kΩ resistor goes in every wire from an ESP32 TX
pin; it protects both sides if a pin direction turns out to be wrong.

| ESP32 pin | Via | Board pin | Signal (EDK500 name) |
| --- | --- | --- | --- |
| GND | | J6-2 (or J5-10, J6-12) | GND |
| GPIO19 | 1 kΩ | J6-15 | TXD0, link A to PIC |
| GPIO18 | 1 kΩ | J6-16 | RXD0, link A from PIC |
| GPIO26 | 1 kΩ | J6-17 | TXD1, link B to PIC |
| GPIO25 | 1 kΩ | J6-18 | RXD1, link B from PIC |
| GPIO33 | 1 kΩ | J5-18 | TXD2, link C to PIC |
| GPIO32 | 1 kΩ | J5-17 | RXD2, link C from PIC |

Do **not** connect J6-10 (RESET_N), J5-11 (+1.8 V) or the JTAG pins.

GPIO21 and GPIO22 are used as dummy TX pins while sniffing; leave them unconnected.

Before wiring, with a multimeter: J6-1 and J6-13 should read 3.3 V, J5-9 about 5 V,
J5-11 about 1.8 V. If not, stop: the header is a different layout from the EDK500.

## Build and flash

Arduino IDE: install the "esp32 by Espressif" board package, open
`pic_probe/pic_probe.ino`, pick "ESP32 Dev Module", upload. Open the serial monitor
at 115200 with "Newline" line ending.

PlatformIO: `pio run -t upload && pio device monitor` from this folder.

## Test procedure

1. **Sniff first (old controller fitted).** Wire it up with the old controller in
   place. Type `sniff`. Swipe a card, press the exit button, open and close the door,
   unlock from the software. Each event should print a `[PIC ]` or `[CTRL]` line.
   Nothing? Try `link B`, then `link C`, then `baud 57600` and so on.
   Copy the whole log and send it over; it tells us which link and command bytes
   the C3 uses.
2. **Scan (old controller removed).** Power off, unplug the old controller, power on.
   Type `scan`. It reports the link and baud rate the PIC answers on and stays there.
3. **Talk.** Try `ver`, `time`, `door`, `beep`, then `unlock 3` to click the lock
   relay for 3 seconds. Swipe a card and watch for a `card on internal reader` line.

The tool feeds the PIC watchdog (command 108) every 10 s in talk mode, as the original
firmware does. Turn it off with `dog off` if you want to see what the PIC does
without it.

## Commands

```
link A|B|C      choose the candidate link (default A)
baud N          choose baud rate (default 115200)
sniff           listen only, old controller still fitted
scan            old controller removed: find the PIC
talk            act as controller on current link/baud
stop            release all pins
ver             ask PIC version            (cmd 20)
time            read PIC clock             (cmd 22)
door            ask door / alarm state     (cmd 110)
beep            200 ms buzzer              (cmd 3)
unlock S        lock relay on for S s, 1-10 (cmd 4)
alarm on|off    alarm relay                (cmd 107)
dog on|off      feed PIC watchdog every 10 s (cmd 108)
raw CMD [B ..]  send any command, decimal bytes
```
