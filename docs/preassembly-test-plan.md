# HPA300 pre-assembly test plan

Run this plan while USB, Boot, Reset, the fan connector, and the TC2030 pads
remain accessible. Record every result. A successful firmware build permits a
controlled bench flash; it does not by itself approve installation or an
unattended overnight run.

Do not seal the enclosure unless every **required** item passes.

## 1. Setup and test record

Have the normal power supply and fan, a LAN-connected computer, native USB,
the intended TC2030 adapter, a logic analyzer or oscilloscope, and a
controllable way to interrupt controller power. Record:

- Date, board revision, MAC address, LAN IP, and COM port
- Git commit/tag, firmware version, and ESP-IDF version
- SHA-256 and byte length of `build/HPA300-FIRMWARE.bin`
- Active OTA slot before and after every update or rollback
- Whether the board is connected to the fan or operating USB-only on the bench

Verify and record the production artifact:

```powershell
Get-FileHash -Algorithm SHA256 build/HPA300-FIRMWARE.bin
Get-Item build/HPA300-FIRMWARE.bin | Select-Object Length, LastWriteTime
espsecure.py verify_signature --version 2 `
  --keyfile secrets/hpa300-ota-signing-key.pem `
  build/HPA300-FIRMWARE.bin
```

Keep the API token out of command history:

```powershell
$HpaIp = "192.168.x.x"
$HpaToken = Read-Host "HPA300 API token"
$HpaAuth = "Authorization: Bearer $HpaToken"
```

Use `curl.exe`, not PowerShell's `curl` alias.

## 2. Controlled first flash -- required

1. Use board #2. Do not perform further write/erase testing on the board whose
   flash no longer responds to ROM-loader commands.
2. Keep the controller out of the purifier and disconnect the fan harness.
   Keep serial logging, Boot, and Reset accessible.
3. Flash the production files from `build/`, which are configured for ESP32-S2,
   4 MB flash, DIO, and 40 MHz flash frequency.
4. Do **not** substitute `HPA300-FIRMWARE-tests.bin`. That is a Unity test
   application, not the signed overnight firmware.
5. Verify the boot log reports the expected partition table, 40 MHz flash,
   the expected firmware version, and `Deterministic fan actor initialized OFF.`
6. Confirm no panic, watchdog reset, boot loop, or unexpected flash warning
   occurs during at least five minutes of USB-only operation.
7. Remove power before connecting the fan harness. Continue the remaining
   tests with the board accessible and the first powered fan run supervised.

## 3. Baseline operation -- required

1. Power-cycle three times. Each time verify the fan starts OFF, no previous
   speed is replayed, and touch control becomes available.
2. Exercise OFF and every speed physically. Verify fan output and LED state.
3. Exercise authenticated REST fan control. A valid request must return HTTP
   `202` with `accepted:true`, `sequence`, and `requested_speed`:

   ```powershell
   # Backslashes preserve JSON quotes through Windows PowerShell's native
   # argument conversion; without them curl receives {speed:2}.
   curl.exe -i -H "$HpaAuth" -H "Content-Type: application/json" `
     -X PUT --data-binary '{\"speed\":2}' "http://$HpaIp/api/v1/fan"
   ```

4. Poll state and verify `speed` is the applied speed, `desired_speed` is the
   latest actor target, and accepted/applied sequence numbers converge:

   ```powershell
   curl.exe -sS -H $HpaAuth "http://$HpaIp/api/v1/state"
   ```

5. Confirm `http://$HpaIp/update` loads.
6. Read OTA status and expect HTTP 200 with `version`, `slot`,
   `maintenance_active:false`, and `busy:false`:

   ```powershell
   curl.exe -sS -H $HpaAuth "http://$HpaIp/api/v1/ota"
   ```

7. Repeat protected requests without a token and with a wrong token. Expect
   `401` for both.

## 4. Deterministic fan-control checks -- required

Record `/api/v1/device` before and after each group.

1. Select a stable speed, then submit that same speed at least 20 times.
   `duplicate_commands` must increase while `transitions_completed` remains
   unchanged. Observe or scope the HC238 enable line; duplicate commands must
   produce no pulse or address change.
2. Send a rapid alternating-speed burst. Intermediate requests may be counted
   as `coalesced_commands`; the final requested state must become stable without
   replaying the discarded sequence afterward.
3. For each real transition, scope the HC238 lines and verify one bounded
   break-before-make sequence: disable, address change, then enable. There must
   be no repeated enable pulse.
4. Verify key 1 cycles OFF/1/2/3/OFF, key 2 toggles turbo/OFF, and key 6 cycles
   the timer only while the fan is active.
5. Verify a REST speed command cancels a local timed-off mode.
6. Power-cycle with the fan at each active speed. Every boot must remain OFF
   until a fresh touch or authenticated REST command arrives.
7. Confirm `control_health.state` remains `ready`, `fault_latched:false`, and
   `deadline_misses:0` throughout the supervised tests. Record
   `max_cycle_us`, `budget_overruns`, and `stack_min_free_bytes`; do not invent
   acceptance limits from this first baseline.

The compiled Unity tests cover duplicate-transition policy, touch-state rules,
tick/sequence wrap, exact disable/address/enable ordering, and injected failure
at all three transition phases. Running the Unity image on a bench test board
is recommended, but it does not replace the scope checks above.

## 5. Low-wear diagnostics -- required

1. After Wi-Fi has settled, capture:

   ```powershell
   curl.exe -sS -H $HpaAuth "http://$HpaIp/api/v1/device"
   ```

2. Verify the response contains `control_health`, `flash_activity`,
   `last_fault`, and `last_event`. Flash counters must say `boot_scoped:true`.
3. Leave the controller idle for at least 30 minutes without provisioning,
   OTA, or deliberate crash testing. Polling telemetry must not itself increase
   flash write or erase counters. Record any unexpected delta as a failure.
4. Repeat with normal fan and REST activity. Fan switching, touch input,
   breadcrumbs, and Home Assistant polling must add no flash writes or erases.
5. Provisioning and OTA are expected to change the counters. Record the before
   and after values so future overnight results have a known comparison.
6. Confirm the retained coredump snapshot exists in the ELF/map and is about
   256 bytes or less. The current implementation is expected to be 208 bytes.

Do not enable full-DRAM dumps, heap poisoning, persistent event logs, or
periodic NVS checkpoints for this test.

## 6. Core-dump retention and capacity -- required before unattended use

Perform this only on the accessible bench. The production firmware deliberately
has no remote crash endpoint; use a debugger or an explicitly test-only build
to induce the watchdog/panic.

1. Keep the matching `build/HPA300-FIRMWARE.elf`.
2. Induce one representative task-watchdog panic and allow the board to reboot.
   Verify it boots OFF and does not replay the fan command.
3. Enter ROM download mode and extract the dump:

   ```powershell
   idf.py -p COMx coredump-info
   ```

4. Verify the dump is complete, decodes against the matching ELF, contains the
   fan-control task and diagnostic snapshot, and fits the 64 KiB partition.
   If it is truncated or absent, stop: expand the coredump partition before an
   overnight run.
5. Save the decoded output, then induce a second panic. Verify the original
   first dump was not overwritten.
6. A retained test dump would prevent an overnight failure from being stored.
   After saving the evidence, verify `partitions-4mb.csv` still places only the
   coredump partition at `0x3d0000` with size `0x10000`, then erase exactly that
   partition while the board is out of the fan:

   ```powershell
   esptool.py --chip esp32s2 -p COMx erase_region 0x3d0000 0x10000
   ```

7. Reboot and confirm `coredump-info` reports no retained dump. Never erase a
   crash dump before extracting it, and never broaden the erase address/size.

## 7. Maintenance authorization -- required

1. Without the physical gesture, attempt a harmless invalid upload. Expect
   `403` and continued normal operation.
2. Complete `4,5,4,5` within ten seconds. Verify maintenance LED behavior and
   that the temporary `HPA300-xxxxxx` AP appears.
3. Read OTA status with the correct LAN token. Expect
   `maintenance_active:true`.
4. Fan control during the open ten-minute authorization window is expected and
   allowed. The actor becomes quiesced only when an OTA, rollback, or
   provisioning flash operation actually begins.
5. During an actual flash operation, the fan must be OFF, touch control must be
   suppressed, and no REST fan command may be applied. If the single HTTP task
   processes a fan request while quiesced it must return `503`; a request queued
   behind the synchronous upload may instead wait or time out, but it must not
   run after the failed/completed maintenance operation.
6. Attempt the upload without a token and with a wrong token. Expect `401` over
   LAN even while maintenance is open.
7. Let one window expire. Expect `maintenance_active:false`, no temporary AP,
   and `403` on another upload.

Prior observation retained for retest: fan control was possible while only the
maintenance window was open. This is now documented as expected behavior; it
is a failure only if control is accepted after the flash operation starts.

## 8. Signed LAN update -- required

1. Record the current slot and turn the fan on.
2. Open maintenance and upload the verified signed production image through
   `/update` or the REST endpoint.
3. During upload verify the actor acknowledges OFF before the first flash
   erase/write, no fan command is applied, touch is suppressed, and the OTA LED
   pattern is shown. A fan request actually serviced during quiescence returns
   `503`; one waiting behind the upload may time out. A throttled upload can
   make this easier to observe.
4. Expect `accepted:true`, then a reboot. Wait at least 40 seconds.
5. Verify the active slot alternated, the old slot is available, maintenance
   closed, Wi-Fi/token survived, and all controls work.
6. Verify the 30-second probation metadata write also occurred while the fan
   was quiesced, then normal fan commands became available again.

Prior recorded build/slot observation: `168c0e9-dirty` booted from `ota_0`.
Retain this only as historical evidence; record the current image independently.

## 9. Invalid image handling -- required

Run each case in a fresh maintenance window. Record HTTP and OTA status before
and after. Every case must retain the running slot, must not reboot, and must
restore normal fan/touch/API operation.

1. `build/HPA300-FIRMWARE-unsigned.bin`: expect `422`.
2. A text file such as `CMakeLists.txt`: expect `422`.
3. A truncated signed binary: expect `422`.
4. An image signed by a temporary different RSA-3072 key: expect `422`.
5. A file larger than 1,920 KiB: expect `413` before any slot change.
6. After each failure, verify maintenance quiescence was released and a fresh
   fan command can be accepted.

## 10. Recovery AP update and rollback -- required

1. Remove the test computer's LAN path to the controller.
2. Perform `4,5,4,5`, join `HPA300-xxxxxx`, and open
   `http://192.168.4.1/update`.
3. Confirm OTA status succeeds without a token through the AP.
4. Upload the signed image without a token. Expect acceptance and reboot into
   the other slot.
5. Rejoin the LAN, wait 40 seconds, and verify slot, credentials, token, and
   controller functions.
6. Repeat through the AP with an unsigned image. Expect `422`, no reboot, and
   no slot change.
7. Confirm `previous_available:true`, open maintenance, and select the previous
   firmware through the UI or `/api/v1/ota/rollback`.
8. Expect fan OFF, controls suppressed, `accepted:true`, and a reboot. Repeat
   once through the recovery AP without a token.

## 11. Interrupted update and probation -- required

Only begin after both slots have booted successfully at least once.

1. Start a throttled signed upload and remove controller power halfway through.
2. Restore power. The last confirmed slot must boot; the partial image must
   never be selected, and the fan must remain OFF until a fresh command.
3. Complete another signed upload. Reset or power-cycle during the new image's
   30-second probation.
4. The bootloader must return to the last confirmed slot. Verify status,
   settings, controls, and no replayed fan state.
5. Complete one uninterrupted update afterward, proving the interrupted slot
   can be overwritten and confirmed normally.

## 12. Network-loss behavior -- required

1. Disconnect the router and reset the controller.
2. Verify fan and local control do not depend on router association.
3. Perform `4,5,4,5`; verify the AP and `/update` remain available.
4. Restore the router and verify automatic reconnection without lost settings.
5. Stall or heavily load HTTP/network processing while monitoring
   `control_health`; local fan control must remain responsive and deadline
   misses must not accumulate.

## 13. Supervised soak and physical recovery -- required

1. Run at least one supervised soak before any unattended test. Exercise fan,
   touch, REST, network loss, and recovery while recording control and flash
   telemetry.
2. During a subsequent overnight run, Home Assistant should retain
   `last_boot`, `runtime`, `control_health`, `flash_activity`, `last_fault`, and
   `last_event`.
3. Treat any unexpected flash write/erase, deadline miss, fault latch, reset,
   unavailable interval, or repeated transition as a failed soak requiring
   investigation before sealing.
4. Recommended endurance test: perform ten alternating signed updates. After
   each cycle record the slot and recheck NVS, token, fan, touch, LEDs, API,
   and flash counters.
5. Use the intended enclosure access to enter ROM download mode and confirm
   `flash_id` succeeds over the final recovery connection.
6. If TC2030 will be the final recovery route, perform an actual signed reflash
   through it, reprovision, and repeat the baseline tests.
7. Photograph and label Boot, Reset, GND, 3V3, TX, and RX. Store the pinout with
   the signing-key recovery documentation.

## Seal/no-seal gate

Seal only after:

- Production image identity/signature was recorded
- Controlled bench flash and supervised fan run passed
- Duplicate requests produced zero HC238 activity
- Every real transition showed one disable/address/enable sequence
- Boot and all reset tests remained OFF with no state replay
- Control telemetry showed no unexplained deadline misses or fault latch
- Idle/local-control telemetry showed no unexplained flash write or erase
- A representative core dump fit, decoded, was preserved, and was cleared
  before the unattended run
- Both OTA slots booted and passed probation
- Signed LAN/AP updates, rejection cases, and rollback passed
- Mid-upload and probation power loss recovered to a confirmed image
- Credentials/token survived every non-erase test
- Boot/Reset and the intended recovery connection were verified
- The signing key has an encrypted off-machine backup

Any unexplained reset, repeated fan switching, fan command accepted during an
active flash operation, rejected upload changing the slot, lost NVS data,
unexpected idle flash wear, undecodable core dump, or inaccessible recovery
connection is **do not seal**.
