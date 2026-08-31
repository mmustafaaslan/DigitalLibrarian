# Verified build environment

This project is intentionally kept on the dependency set already proven to
compile for the target hardware. Do not upgrade the board core or libraries as
part of an unrelated change.

The audit build on 2026-08-31 used:

- Arduino IDE 2.x bundled `arduino-cli`
- Espressif ESP32 board package 3.3.5
- Board: Waveshare ESP32-S3 Touch LCD 7
- ArduinoJson 7.4.2
- FastLED 3.10.3
- TJpg_Decoder 1.1.0
- ESP32_Display_Panel 0.1.6
- ESP32_IO_Expander 0.1.0
- LVGL 8.4.0

The exact fully qualified board name and options are stored in
`tools/verify-build.ps1`. That script only compiles; it does not install,
upgrade, upload, or erase anything.

From PowerShell at the repository root:

```powershell
.\tools\verify-build.ps1
```

The verified target uses QIO flash, 8 MB flash, Huge APP partitioning, enabled
PSRAM, 240 MHz CPU, loop core 1, event core 1, and no erase-before-upload.

## Arduino IDE

Select **Waveshare ESP32-S3 Touch LCD 7** and mirror the options above. Compile
with **Sketch > Verify/Compile** before connecting the board. Keep a copy of
the currently working binary or git commit before changing any library.
