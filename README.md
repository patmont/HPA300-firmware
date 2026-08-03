# HPA300 Firmware

Custom ESP32-S2 firmware for an HPA300 HEPA filter controller. The controller
provides a capacitive touch interface, PWM-dimmable status LEDs, and mutually
exclusive fan selection through a 74HC238 decoder.

## Supported environment

- ESP-IDF v5.5.1
- ESP32-S2 target

The target is recorded in `sdkconfig.defaults`, and the development container is
pinned to ESP-IDF v5.5.1. A normal command-line build is:

```text
idf.py set-target esp32s2
idf.py build
```

The board mapping/invariant test application can be built separately:

```text
idf.py -C test -B ../build-test build
```

## Safety model

The 74HC238 provides the hardware single-active-output invariant. Firmware
disables the decoder before changing its address and initializes fan control
before touch or LED peripherals. The active-high E3 input is held inactive by
R18, a 10 kΩ hardware pull-down, so fan outputs remain disabled before the
ESP32-S2 starts.

## Board mapping

Pin assignments live in `components/board/include/board.h`. The mappings below
were verified against the `HPA300-hardware` KiCad schematic and PCB. KiCad's
schematic-parity check reports no differences between them.

Current touch mapping:

| Logical key | Touch channel / GPIO |
| --- | --- |
| 1 | 3 |
| 2 | 2 |
| 3 | 1 |
| 4 | 5 |
| 5 | 6 |
| 6 | 4 |

Current controls:

| Key | Action |
| --- | --- |
| 1 | Cycles `off → fan 1 → fan 2 → fan 3 → off`. From fan 4, selects off. |
| 2 | Toggles fan 4 and off. From any other active speed, selects fan 4. |
| 3 | Directly cycles unified LED brightness: High (100%), Medium (50%), Low (5%), Off (0%). |
| 1, 2, 4, 5, or 6 | If the intended brightness is below High, temporarily sets LEDs to High for five seconds after the last non-dimmer touch, then restores the intended brightness. |

Crossing the fan Off position in either direction resets the intended LED brightness to High. For example, after an extended idle period at Off, key 1 selects fan 1 and leaves the LEDs at High.

Current HC238 mapping:

| Fan speed | HC238 output |
| --- | --- |
| 1 | Y0 |
| 2 | Y2 |
| 3 | Y4 |
| 4 | Y6 |

HC238 A0, E1, and E2 are tied to ground. A1 is GPIO18, A2 is GPIO17,
and active-high E3 is GPIO16. R16, R17, and R18 provide 10 kΩ pull-downs
on those three MCU-controlled inputs. Decoder outputs Y0, Y2, Y4, and Y6
drive FAN1-FAN4 respectively through channels 1, 3, 5, and 7 of the TBD62783A.

Verified LED mapping:

| LED | GPIO | Function |
| --- | --- | --- |
| LED1 | 12 | Fan speed 3 |
| LED2 | 15 | Check filters |
| LED3 | 8 | 8-hour timer |
| LED4 | 13 | Fan speed 2 |
| LED5 | 7 | Check pre-filter |
| LED6 | 9 | 4-hour timer |
| LED7 | 14 | Fan speed 1 |
| LED8 | 11 | Fan speed 4 |
| LED9 | 10 | 2-hour timer |
