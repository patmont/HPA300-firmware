# Building HPA300 firmware

The project uses ESP-IDF v5.5.1 and targets ESP32-S2. The devcontainer is the
canonical reproducible build environment on Windows, Linux, and macOS. A native
ESP-IDF installation is recommended for flashing and monitoring hardware.

## Fastest setup: VS Code devcontainer

Install Docker, Visual Studio Code, and the Dev Containers extension. Clone the
repository, open it in VS Code, and choose **Dev Containers: Reopen in
Container**. In the container terminal, run:

```text
python tools/project.py prepare
idf.py build
idf.py -C test -B build-test build
```

`prepare` verifies ESP-IDF and creates an ignored local RSA-3072 development
key if one does not already exist. It never replaces a key. Back up any key
used to flash a real controller: that controller will accept future OTA images
only when they are signed by the same key.

The test command builds the ESP32-S2 Unity test application. Running those
tests still requires flashing it to a bench controller:

```text
idf.py -C test -B build-test -p PORT flash monitor
```

Do not flash the test application on an installed or unattended purifier.

CI builds this same `.devcontainer` configuration, then builds the production
firmware and test application inside it. A passing firmware job therefore
checks both the documented setup and a clean containerized rebuild.

## Native setup

Install exactly ESP-IDF v5.5.1 using Espressif's platform instructions:

- [Windows installation](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s2/get-started/windows-setup.html)
- [Linux and macOS installation](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s2/get-started/linux-macos-setup.html)

Open an activated ESP-IDF shell, change to this repository, and run the same
commands used in the devcontainer:

```text
python tools/project.py prepare
idf.py build
idf.py -C test -B build-test build
```

Confirm the environment at any time with:

```text
python tools/project.py doctor
```

ESP-IDF uses `COMx` device names on Windows and `/dev/tty*` device names on
Linux and macOS. Supply the detected port when interacting with hardware:

```text
idf.py -p PORT flash monitor
```

Direct USB/serial passthrough from a container is host-dependent. Native tools
are the supported default for flashing and monitoring on Windows and macOS.

## Build outputs and clean rebuilds

The production application build creates:

- `build/HPA300-FIRMWARE-unsigned.bin`
- `build/HPA300-FIRMWARE.bin` (signed with the local key)
- `build/HPA300-FIRMWARE.elf` (needed to decode a matching coredump)

Generated build directories, `sdkconfig`, and `secrets/` are ignored by Git.
To prove a clean rebuild without deleting source files or keys, run:

```text
idf.py fullclean
idf.py build
```

Never publish a development key. CI creates an ephemeral key only to prove that
the signed-image build works; CI-produced firmware is not an official release.

## Release builds

Release images must come from a clean, exact `v*` tag and use the central
offline release key described in [docs/signing.md](docs/signing.md). Verify the
resulting binary before flashing or publishing:

```text
espsecure.py verify_signature --version 2 --keyfile secrets/hpa300-ota-signing-key.pem build/HPA300-FIRMWARE.bin
```

On Windows, the executable may be named `espsecure.exe`.
