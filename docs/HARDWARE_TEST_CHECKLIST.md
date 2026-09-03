# Hardware test checklist

Use this checklist after a firmware change. It avoids destructive storage tests
and verifies the features most likely to expose timing, power, or persistence
problems.

## Before upload

- Back up the SD card and note the current working git commit.
- Disconnect external LED power while wiring. Confirm strip direction and the
  labels printed on the strip; wire colours are not standardized.
- Connect LED ground to ESP32 ground, LED data input to GPIO 6 through a
  330–470 ohm series resistor, and LED positive to a correctly sized 5 V supply.
- For more than a few pixels, do not power the strip from the board. Use a fused
  external supply, common ground, a capacitor across 5 V/GND near the strip,
  and power injection appropriate to the strip length.
- In Settings, enter the actual pixel count and a conservative power limit. The
  firmware defaults to a 3000 mA FastLED limit.

## Smoke test

1. Compile with `tools/verify-build.ps1`, then upload with Arduino IDE.
2. Confirm the serial console prints the upload smoke-test line and reaches
   `BOOT COMPLETE` without repeated resets.
3. Confirm the existing CD and book counts load from the SD card.
4. Navigate forward/back rapidly and confirm covers, labels, and buttons remain
   responsive.
5. Open Search, tap the text field, verify the on-screen keyboard appears, and
   search by title, artist/author, and genre.
6. Toggle Favourite several times. Confirm immediate visual feedback and verify
   the final state survives a reboot.
7. Add a temporary record, edit it, reboot, and delete it. Confirm no duplicate
   ID or orphaned record appears.
8. Test an invalid year, out-of-range LED number, and duplicate unique ID. Each
   must show a validation error without changing the saved record.
9. Open Tracklist/Lyrics and start a single lyrics fetch. Confirm navigation
   remains usable while the request runs.

## Web test

1. Open `http://mylibrary.local/browse` (or the configured mDNS name shown on the device) in a private browser window.
   Confirm the library is not visible before login.
2. Log in once, then visit Browse, Scanner, Covers, Backup, Manual, and Errors.
   Confirm the PIN is not added to the browser URL.
3. Select an item, toggle favourite, edit LEDs, and scan one known barcode.
4. Try a wrong PIN and confirm protected APIs return 401.
5. Confirm restart requires a POST action and a valid PIN.
6. Confirm the storage-test button reports that tests are disabled in a normal
   firmware build.

## Storage interruption test

Only perform this on a spare SD card containing a disposable copy of the
library. Interrupt power during a record edit, reboot, and verify either the old
or new complete JSON file remains readable. Never run the destructive
`StorageTests` suite on the only copy of a library.

## Acceptance

- No crash, watchdog reset, corrupt JSON, missing keyboard, overlapping labels,
  or multi-second favourite-button stall.
- LED output stays within the configured pixel count and power limit.
- A reboot preserves the last successfully confirmed edit.
