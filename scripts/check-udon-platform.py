#!/usr/bin/env python3
"""Static checks for the Udon platform package and firmware bootstrap."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:
    sys.exit("check-udon-platform.py requires Python 3.11 or newer")

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "Platforms/OnePlus/udonPkg"
ERRORS: list[str] = []


def require(path: Path) -> str:
    if not path.is_file():
        ERRORS.append(f"missing required file: {path.relative_to(ROOT)}")
        return ""
    return path.read_text(errors="replace")


for relative in (
    "Platforms/OnePlus/udonPkg/udon.dsc",
    "Platforms/OnePlus/udonPkg/udon.fdf",
    "Platforms/OnePlus/udonPkg/udon.dec",
    "Platforms/OnePlus/udonPkg/DeviceBuild.py",
    "Platforms/OnePlus/udonPkg/Drivers/ShengBootMemoryDxe/ShengBootMemoryDxe.c",
    "Platforms/OnePlus/udonPkg/Drivers/ShengBootMemoryDxe/ShengBootMemoryDxe.inf",
    "Platforms/OnePlus/udonPkg/Applications/UdonLogExport/UdonLogExport.c",
    "Platforms/OnePlus/udonPkg/Applications/UdonLogExport/UdonLogExport.inf",
    "Platforms/OnePlus/udonPkg/FdtBlob/udon-test.dtb",
    "Platforms/OnePlus/udonPkg/Library/MemoryMapLib/MemoryMapLib.c",
    "Platforms/OnePlus/udonPkg/Library/ConfigurationMapLib/ConfigurationMapLib.c",
    "Resources/Configs/udon.toml",
    "scripts/build-udon-firmware.sh",
):
    require(ROOT / relative)

config = require(ROOT / "Resources/Configs/udon.toml")
if config:
    parsed = tomllib.loads(config)
    fd = parsed.get("uefi_fd", {})
    if fd.get("base") != 0xA7100000 or fd.get("size") != 0x400000:
        ERRORS.append("udon.toml uefi_fd must be 0xA7100000/0x400000")
    boot = parsed.get("boot_image", {})
    if boot.get("header_version") != 4:
        ERRORS.append("udon.toml boot_image.header_version must be 4")

dsc = require(PACKAGE / "udon.dsc")
for text in (
    "PLATFORM_NAME                  = udon",
    "SOC_TYPE                       = 0",
    "USE_CUSTOM_DISPLAY_DRIVER      = 0",
    "UniXNUPkg/Application/UniXNU/UniXNU.inf",
    "UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf",
    "DarwinBootPkg/Driver/DarwinBoot.inf",
    "udonPkg/Drivers/ShengBootMemoryDxe/ShengBootMemoryDxe.inf",
    "udonPkg/Applications/UdonLogExport/UdonLogExport.inf",
):
    if dsc and text not in dsc:
        ERRORS.append(f"udon.dsc is missing: {text.strip()}")

fdf = require(PACKAGE / "udon.fdf")
dxe = require(PACKAGE / "Include/DXE.inc")
fdf_components = fdf + "\n" + dxe
for text in (
    "!include Include/APRIORI.inc",
    "!include Include/DXE.inc",
    "!include Include/RAW.inc",
    "UniXNUPkg/Application/UniXNU/UniXNU.inf",
    "DarwinBootPkg/Driver/DarwinBoot.inf",
):
    if fdf_components and text not in fdf_components:
        ERRORS.append(f"udon.fdf is missing: {text}")

memory_map = require(PACKAGE / "Library/MemoryMapLib/MemoryMapLib.c")
for text in ("UEFI FD", "0xA7100000", "0x00300000", "Display Reserved"):
    if memory_map and text not in memory_map:
        ERRORS.append(f"MemoryMapLib is missing: {text}")

unexpected = []
for path in PACKAGE.rglob("*"):
    if path.is_file() and path.suffix.lower() in {".bak", ".efi", ".depex", ".bin", ".img"}:
        unexpected.append(str(path.relative_to(ROOT)))
if unexpected:
    ERRORS.append("unexpected binary/backup artifacts: " + ", ".join(unexpected))

result = subprocess.run(
    ["bash", "-n", str(ROOT / "scripts/build-udon-firmware.sh")],
    cwd=ROOT,
    text=True,
    capture_output=True,
)
if result.returncode:
    ERRORS.append("build-udon-firmware.sh has a shell syntax error")

vendor = ROOT / "Binaries/udon"
if not vendor.is_dir():
    print("note: Binaries/udon is absent; firmware export requires a user-supplied PHK110 dump")

if ERRORS:
    print("Udon UEFI check failed:", file=sys.stderr)
    for error in ERRORS:
        print(f"- {error}", file=sys.stderr)
    sys.exit(1)

print("Udon UEFI check passed")
