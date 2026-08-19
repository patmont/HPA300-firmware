#!/usr/bin/env python3
"""Small, dependency-free helpers for preparing an HPA300 build checkout."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
SIGNING_KEY = ROOT / "secrets" / "hpa300-ota-signing-key.pem"
EXPECTED_IDF_VERSION = "v5.5.1"


def find_tool(*names: str) -> str | None:
    """Return the first named executable available on PATH."""
    for name in names:
        if path := shutil.which(name):
            return path
    return None


def idf_version() -> tuple[str | None, bool]:
    """Read and validate the active ESP-IDF version."""
    idf = find_tool("idf.py", "idf.py.exe")
    if idf is None:
        return None, False
    result = subprocess.run(
        [idf, "--version"],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    version = (result.stdout or result.stderr).strip()
    return version, result.returncode == 0 and EXPECTED_IDF_VERSION in version


def doctor(require_key: bool = True) -> bool:
    """Report whether the checkout has everything needed to build."""
    healthy = True
    version, version_ok = idf_version()
    if version is None:
        print("[missing] idf.py (activate ESP-IDF or open the devcontainer)")
        healthy = False
    elif version_ok:
        print(f"[ok] {version}")
    else:
        print(f"[wrong version] {version}; expected {EXPECTED_IDF_VERSION}")
        healthy = False

    if find_tool("git", "git.exe"):
        print("[ok] git")
    else:
        print("[missing] git")
        healthy = False

    if SIGNING_KEY.is_file():
        print(f"[ok] development signing key: {SIGNING_KEY.relative_to(ROOT)}")
    elif require_key:
        print("[missing] development signing key; run: python tools/project.py prepare")
        healthy = False
    else:
        print("[pending] development signing key will be created")

    return healthy


def create_development_key() -> bool:
    """Create the ignored local signing key without ever replacing one."""
    if SIGNING_KEY.exists():
        print(f"[ok] keeping existing key: {SIGNING_KEY.relative_to(ROOT)}")
        return True

    espsecure = find_tool("espsecure.py", "espsecure.exe", "espsecure")
    if espsecure is None:
        print("[missing] espsecure (activate ESP-IDF or open the devcontainer)")
        return False

    SIGNING_KEY.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            espsecure,
            "generate_signing_key",
            "--version",
            "2",
            "--scheme",
            "rsa3072",
            str(SIGNING_KEY),
        ],
        cwd=ROOT,
        check=True,
    )
    print(f"[created] {SIGNING_KEY.relative_to(ROOT)}")
    print(
        "Back up keys used on real devices; future OTA images must use the same key."
    )
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "command",
        choices=("doctor", "prepare"),
        help="check the environment, optionally creating an ignored development key",
    )
    args = parser.parse_args()

    if args.command == "doctor":
        return 0 if doctor() else 1

    if not doctor(require_key=False):
        return 1
    try:
        if not create_development_key():
            return 1
    except subprocess.CalledProcessError as error:
        print(f"Key generation failed with exit code {error.returncode}", file=sys.stderr)
        return error.returncode or 1
    return 0 if doctor() else 1


if __name__ == "__main__":
    raise SystemExit(main())
