# FSR mat tester app

Flutter Android app for the 7-channel FSR blanket production check.

The app scans for BLE devices named `FSR-MAT-XXXX`, sorts the filtered list by
RSSI from strongest to weakest, lets the operator choose the board manually,
then subscribes to the FSR notification characteristic and highlights head,
hands, knees, and feet when the corresponding ADC channel is below `3000`.

## BLE contract

Service UUID: `2f3a0001-3c18-4b22-9c70-364d4f535200`

Notify characteristic UUID: `2f3a0002-3c18-4b22-9c70-364d4f535200`

Payload order:

1. HEAD
2. HAND_R
3. HAND_L
4. KNEE_R
5. KNEE_L
6. FOOT_R
7. FOOT_L

## Common commands

```powershell
flutter pub get
flutter run
flutter build apk
```
