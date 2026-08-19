# Firmware signing policy

HPA300 uses RSA-3072 Secure Boot v2 signatures to validate OTA application
updates without enabling hardware Secure Boot. The private key signs an image
during the build. The public key in the currently running image authorizes the
next OTA image, so a device stays on one signing-key chain until a complete
image is physically flashed over USB/UART.

This protects OTA updates from unsigned or differently signed network images.
It does not prevent someone with physical flash access from installing a new
bootloader and establishing a different signing-key chain.

## Development keys

The normal development workflow uses:

```text
secrets/hpa300-ota-signing-key.pem
```

That path is ignored by Git. Create it with:

```text
python tools/project.py prepare
```

This key is suitable for local development and bench images. Treat it as a
development key unless it has deliberately been designated as the official
release key. Never publish it.

## Official `/patmont/` releases

Official releases use one central RSA-3072 private key controlled only by the
project owner. Store that key outside the repository, for example:

```text
C:\Users\YOUR_USERNAME\HPA300-secrets\hpa300-release-key.pem
```

Keep two encrypted backups. Do not commit it, place it in the devcontainer, or
add it to GitHub Actions. CI uses an ephemeral key only to prove that a signed
image can be built; CI output is not an official release.

For an official release, configure the local ignored `sdkconfig` to use the
external key through `idf.py menuconfig`:

```text
Security features → Secure boot private signing key
```

Then build and verify on the trusted release machine:

```text
idf.py fullclean
idf.py build
espsecure.py verify_signature --version 2 --keyfile C:\Users\YOUR_USERNAME\HPA300-secrets\hpa300-release-key.pem build/HPA300-FIRMWARE.bin
```

Record the release tag, SHA-256, byte length, and signature verification result
before publishing the image.

## User-custom firmware and forks

Anyone may create a personal signing chain. Generate a key in the fork's
ignored `secrets/` directory, build with it, and use USB/UART to flash the
complete image:

```text
espsecure.py generate_signing_key --version 2 --scheme rsa3072 secrets/hpa300-ota-signing-key.pem
idf.py fullclean build
```

After that physical flash, OTA images must be signed with that user's key.
Official `/patmont/` OTA images will not be accepted until the official image
is physically flashed again. This is intentional: it allows experimentation
and alternate firmware without granting anyone the ability to publish an
officially trusted OTA image.

Do not overwrite or discard the key that a device currently trusts unless a
physical recovery flash is planned. Back up any key used on a real controller.
