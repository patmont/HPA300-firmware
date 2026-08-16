# HPA300 pre-assembly test plan

Run this while USB, Boot, Reset, and the TC2030 pads remain accessible. Record
the result of every test. Do not seal the enclosure unless all **required**
tests pass.

## 1. Setup and test record

Have the normal power supply and fan, a LAN-connected computer, native USB,
the intended TC2030 adapter, and a controllable way to interrupt controller
power. Record:

- Date, board revision, MAC address, LAN IP, and COM port
- Git commit/tag and firmware version
- SHA-256 of `build/HPA300-FIRMWARE.bin`
- Active slot before and after every update or rollback

In PowerShell, keep the token out of command history:

```powershell
$HpaIp = "192.168.x.x"
$HpaToken = Read-Host "HPA300 API token"
$HpaAuth = "Authorization: Bearer $HpaToken"
```

Use `curl.exe`, not PowerShell's `curl` alias, below.

## 2. Baseline operation — required

1. Power-cycle three times. Each time, verify the fan starts off and touch
   control becomes available.
2. Exercise fan off and every speed physically. Verify LED state and fan output.
3. Exercise authenticated REST fan control.
4. Confirm `http://$HpaIp/update` loads.
5. Read OTA status:

   ```powershell
   curl.exe -sS -H $HpaAuth "http://$HpaIp/api/v1/ota"
   ```

   Expect HTTP 200 with `version`, `slot`, `maintenance_active:false`, and
   `busy:false`.
6. Repeat without a token and with a wrong token:

   ```powershell
   curl.exe -sS -o NUL -w "%{http_code}`n" "http://$HpaIp/api/v1/ota"
   curl.exe -sS -o NUL -w "%{http_code}`n" -H "Authorization: Bearer wrong" "http://$HpaIp/api/v1/ota"
   ```

   Expect `401` for both.

## 3. Maintenance authorization — required

1. Without the gesture, attempt a harmless invalid upload:

   ```powershell
   curl.exe -sS -o NUL -w "%{http_code}`n" -X POST -H $HpaAuth `
     -H "Content-Type: application/octet-stream" --data-binary "not-firmware" `
     "http://$HpaIp/api/v1/ota"
   ```

   Expect `403` and continued normal operation.
2. Complete `4,5,4,5` within ten seconds. Verify the maintenance LED behavior
   and that the temporary `HPA300-xxxxxx` AP appears.
3. Read OTA status with the correct LAN token. Expect
   `maintenance_active:true`.
4. Attempt the upload without a token and with a wrong token. Expect `401` over
   LAN even while maintenance is active.
5. Let one window expire. After ten minutes, expect
   `maintenance_active:false`, no temporary AP, and `403` on another upload.
   !! I was able to control fan during maintenance period whether this is allowed or not.

## 4. Signed LAN update — required

Verify the artifact first:

```powershell
espsecure.py verify_signature --version 2 `
  --keyfile secrets/hpa300-ota-signing-key.pem `
  build/HPA300-FIRMWARE.bin
Get-FileHash -Algorithm SHA256 build/HPA300-FIRMWARE.bin
```

1. Record the current slot. Turn the fan on and open maintenance.  168c0e9-dirty from ota_0
2. Upload through `/update`, or use:

   ```powershell
   curl.exe -sS -H $HpaAuth -H "Content-Type: application/octet-stream" `
     --data-binary "@build/HPA300-FIRMWARE.bin" `
     "http://$HpaIp/api/v1/ota"
   ```

3. During upload, verify the fan turns off, controls are suppressed, and the
   OTA LED pattern is shown. `--limit-rate 25k` can make this easier to observe.
4. Expect `accepted:true`, then a reboot. Wait at least 40 seconds.
5. Read status and verify the active slot alternated, the old slot is listed as
   available, maintenance closed, Wi-Fi/token survived, and all controls work.

## 5. Invalid image handling — required

Run each case in a fresh maintenance window. Record HTTP status and OTA status
before and after. Every case must retain the running slot, must not reboot, and
must restore normal fan/touch/API operation.

1. `build/HPA300-FIRMWARE-unsigned.bin`: expect `422`.
2. A text file such as `CMakeLists.txt`: expect `422`.
3. A truncated copy of the signed binary: expect `422`.
4. An image signed by a temporary, different RSA-3072 key: expect `422`. Never
   replace the real key to make this artifact.
5. A file larger than 1,920 KiB: expect `413` before any boot-slot change.

## 6. Recovery AP update — required

1. Remove the test computer's LAN path to the controller.
2. Perform `4,5,4,5`, join `HPA300-xxxxxx`, and open
   `http://192.168.4.1/update`.
3. Confirm `GET http://192.168.4.1/api/v1/ota` succeeds without a token.
4. Upload the signed image without a token. Expect acceptance and reboot into
   the other slot.
5. Rejoin the LAN, wait 40 seconds, and verify slot, credentials, token, and
   all controller functions.
6. Repeat through the AP with the unsigned image. Expect `422`, no reboot, and
   no boot-slot change.

## 7. Manual rollback — required

1. Confirm status says `previous_available:true`, then open maintenance.
2. Use **Boot previous firmware**, or:

   ```powershell
   curl.exe -sS -X POST -H $HpaAuth "http://$HpaIp/api/v1/ota/rollback"
   ```

3. Expect `accepted:true`, fan off, and reboot. After 40 seconds verify the
   previous slot is active and settings survived.
4. Repeat once from the recovery AP without a token.

## 8. Interrupted update and probation — required

Only begin after both slots have booted successfully at least once.

1. Start a throttled signed upload and remove controller power halfway through.
2. Restore power. The last confirmed slot must boot; the partial image must
   never be selected.
3. Complete another signed upload. Reset or power-cycle during the new image's
   30-second probation.
4. The bootloader must return to the last confirmed slot. Verify through status
   and recheck settings and controls.
5. Complete one uninterrupted update afterward, proving the interrupted slot
   can be overwritten and confirmed normally.

## 9. Network-loss behavior — required

1. Disconnect the router and reset the controller.
2. Verify fan and local control do not depend on router association.
3. Perform `4,5,4,5`; verify the AP and `/update` remain available.
4. Restore the router and verify automatic reconnection without lost settings.

## 10. Endurance and physical recovery

1. **Recommended:** perform ten alternating signed updates. After every cycle,
   record the slot and recheck NVS credentials, token, fan, touch, LED, and API.
2. **Required:** use the intended enclosure access to enter ROM download mode:

   ```powershell
   esptool.py --chip esp32s2 -p COMx flash_id
   ```

3. Native-USB erase/reflash was exercised during initial migration. If TC2030
   will be the final recovery route, perform an actual erase/signed reflash over
   TC2030, reprovision, and repeat the baseline tests.
4. Photograph and label Boot, Reset, GND, 3V3, TX, and RX. Store the pinout with
   the signing-key recovery documentation.

## Seal/no-seal gate

Seal only after:

- Both slots booted and passed probation
- Signed LAN and AP updates passed
- Invalid and oversized images were rejected without a slot change
- Manual and automatic rollback passed
- Mid-upload power loss recovered to a confirmed image
- Credentials/token survived every non-erase test
- Fan-off behavior was observed during upload, reboot, and rollback
- Boot/Reset and the intended recovery connection were verified
- The signing key has an encrypted off-machine backup

Any unexplained reset, rejected upload changing the boot slot, fan activation
during OTA, lost NVS data, or inaccessible recovery connection is **do not
seal**.
