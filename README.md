# FSR blanket production tester

This workspace contains two projects:

- `firmware/`: PlatformIO Arduino firmware for ESP32-WROOM.
- `app/`: Flutter Android app that displays the seven FSR positions.

The tester uses BLE notifications. A channel is highlighted when its averaged
ADC value is lower than `3000`.
