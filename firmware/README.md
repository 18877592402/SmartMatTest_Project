# FSR blanket firmware

PlatformIO + Arduino firmware for the ESP32-WROOM board.

## Pin map

| Signal | ESP32 GPIO |
| --- | --- |
| MUX_ADC | IO35 |
| MUX_S0 | IO32 |
| MUX_S1 | IO33 |
| MUX_S2 | IO27 |
| FSR_EN | IO14 |
| KEY_PWR | IO13 |
| VBUS_DET | IO23 |
| BOOST_EN | IO21 |
| AMP_SD | IO5 |
| LED_R | IO17 |
| LED_G | IO18 |
| LED_B | IO19 |

The firmware assumes `FSR_EN` is active-low. If the sensor power switch on the
final board is active-high, change `kFsrPowerActiveLevel` in `src/main.cpp` to
`HIGH`.

The RGB LED is configured as active-low because the schematic shows a common
anode tied to `3V3`. If the final board uses a common-cathode LED, change
`kRgbLedActiveLow` in `src/main.cpp` to `false`.

## RGB status

| State | RGB behavior |
| --- | --- |
| Powered, not connected | Blue breathing |
| Connected and charging (`VBUS_DET` high) | Green breathing |
| Connected and not charging | Blue solid |

LED priority is blue breathing, then green breathing, then blue solid.

## Power key

`KEY_PWR` on `IO13` is active-low.

- Runtime: hold `KEY_PWR` for 2 seconds to enter ESP32 deep sleep.
- Deep sleep: hold `KEY_PWR` for 2 seconds to confirm wake. Releasing early
  returns the board to deep sleep.

Before deep sleep the firmware disables FSR sensor power, disables boost
(`BOOST_EN` low), disables the amplifier (`AMP_SD` low), and turns off the RGB
LED.

## Sensor order

BLE payload bit and ADC order:

1. HEAD
2. HAND_R
3. HAND_L
4. KNEE_R
5. KNEE_L
6. FOOT_R
7. FOOT_L

An FSR is treated as triggered when its averaged ADC value is below `3000`.

## BLE protocol

Device name: `FSR-MAT-XXXX`

`XXXX` is the last four hex digits of the ESP32 Bluetooth MAC address.

Service UUID: `2f3a0001-3c18-4b22-9c70-364d4f535200`

Notify characteristic UUID: `2f3a0002-3c18-4b22-9c70-364d4f535200`

Control characteristic UUID: `2f3a0003-3c18-4b22-9c70-364d4f535200`

Notification payload is 16 bytes:

| Byte | Meaning |
| --- | --- |
| 0 | Header `0xA5` |
| 1 | Trigger bit mask |
| 2..15 | Seven little-endian uint16 ADC values |

Write ASCII `D` to the control characteristic to ask the ESP32 to disconnect
the current BLE central. The app uses this before its local disconnect cleanup
because some Android BLE stacks keep the GATT link alive for too long.

## Common commands

```powershell
pio run
pio run -t upload
pio device monitor
```
