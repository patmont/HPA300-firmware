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
- Confirm the inactive-slot erase and upload complete without a task-watchdog
  reset or a browser network error.
- After the OTA reboot and probation, repeatedly reload the update page and
  refresh its status; signed rollback-image validation must not panic or reset.
- Confirm the AP accepts a signed image without the token but rejects unsigned
  and differently signed images.
- Confirm wrong-project, wrong-chip, malformed, truncated, and oversized images
  do not change the configured boot partition.
- Confirm a failed upload leaves the fan off but restores touch and API control.
- Upload the same signed image as the running version. Confirm it enters the
  normal 30-second probation, is marked valid without a second reboot, and the
  update/status pages remain refreshable throughout.
- While repeatedly refreshing `/api/v1/ota`, verify `previous_validation`
  changes from `pending` to `valid`, `invalid`, or `error` and that no image
  validation runs on the HTTP task.
- Record `service_health.ota_worker.minimum_free_stack_bytes` after signed,
  unsigned, truncated, and rollback validation. It must remain at least 2048.

## Boot and power-loss recovery

- Remove power during inactive-slot erase, early/mid/late upload, after image
  validation, and during the 30-second probation period.
- At each point, confirm either the last valid image boots or the new image
  enters probation and can automatically return to the last valid image.
- Force a critical initialization failure and a probation-period reset; confirm
  bootloader rollback occurs.
- Repeat probation with the router unavailable and by disconnecting an
  established connection. Confirm both cases perform a planned rollback and
  boot OFF.
- Use **Boot previous firmware** from both LAN and AP and verify the selected
  image itself receives rollback protection during its probation.

## Endurance and final recovery

- Perform at least ten successful updates, confirming the active slot alternates
  between `ota_0` and `ota_1` and NVS credentials survive every normal OTA.
- Confirm the fan is disabled during every upload, reset, and rollback.
- Finally enter ROM download mode through the intended enclosure service access,
  run a complete UART/USB erase-and-flash recovery, and reprovision the device.

## Fault containment

- Inject LED and touch initialization failures independently. Verify the fan
  actor and REST control remain responsive at the current speed, the failed
  service retries after 1, 5, 30, and then 60 seconds, and the device does not
  reboot.
- Inject HTTP-start and captive-DNS bind failures. Verify local fan/touch
  control continues, the service remains owned and retryable, and no DNS task
  or allocation is leaked after shutdown.
- Interrupt an upload during preflight, erase, write, and final validation.
  Verify bounded buffers return to the pool, the fan stays OFF, and a later
  maintenance session can update successfully without rebooting first.
- Flood authenticated and unauthenticated API calls while changing fan speeds.
  Require zero unexplained `deadline_misses`; compare fan, HTTP, OTA-worker,
  diagnostics, and manager stack minima before and after the run.
- Force a fan transition backend error. Verify it immediately disables output,
  latches the phase/error, rejects commands, and does not retry the transition.

## Coredump evidence

- Confirm `/api/v1/device` reports `coredump_capacity_bytes` as 196608 and
  reports `coredump_present`, `coredump_readable`, and the retained dump size.
- Capture representative HTTP, OTA-worker, and fan-watchdog failures. Read the
  first retained dump in programming mode with the matching ELF and verify each
  binary fits the 192 KiB partition.
- After archiving a dump, erase only the coredump partition, reproduce the next
  fault, and confirm first-dump preservation no longer masks new evidence.
