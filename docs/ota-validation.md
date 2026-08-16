# OTA validation checklist

Run this checklist on the packaged hardware candidate before making USB/UART
access difficult. Record firmware versions, active slots, and observed results.

## Clean installation

- Enter ROM download mode and record `esptool.py --chip esp32s2 -p COMx flash_id`.
- Confirm the selected partition CSV does not exceed the reported capacity.
- Build from a clean tagged commit; verify the signed image and confirm the
  unsigned image fails `espsecure.py verify_signature`.
- Run `idf.py erase-flash`, `idf.py flash monitor`, and confirm the initial app
  boots from `ota_0` with Secure Boot and flash encryption disabled.
- Reprovision Wi-Fi and the API token; verify local touch and REST fan control.

## Authorization and validation

- Without the physical gesture, confirm LAN and AP OTA writes return `403`.
- During maintenance, confirm LAN writes without the bearer token return `401`.
- During maintenance, upload a signed image over LAN and over the recovery AP.
- Confirm the AP accepts a signed image without the token but rejects unsigned
  and differently signed images.
- Confirm wrong-project, wrong-chip, malformed, truncated, and oversized images
  do not change the configured boot partition.
- Confirm a failed upload leaves the fan off but restores touch and API control.

## Boot and power-loss recovery

- Remove power during inactive-slot erase, early/mid/late upload, after image
  validation, and during the 30-second probation period.
- At each point, confirm either the last valid image boots or the new image
  enters probation and can automatically return to the last valid image.
- Force a critical initialization failure and a probation-period reset; confirm
  bootloader rollback occurs.
- Repeat probation with the router unavailable; confirm actual router
  association is not required and the physical recovery AP remains available.
- Use **Boot previous firmware** from both LAN and AP and verify the selected
  image itself receives rollback protection during its probation.

## Endurance and final recovery

- Perform at least ten successful updates, confirming the active slot alternates
  between `ota_0` and `ota_1` and NVS credentials survive every normal OTA.
- Confirm the fan is disabled during every upload, reset, and rollback.
- Finally enter ROM download mode through the intended enclosure service access,
  run a complete UART/USB erase-and-flash recovery, and reprovision the device.
