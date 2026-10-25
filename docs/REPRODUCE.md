# Reproducing the Udon UEFI

The source tree is complete, but the PHK110 vendor modules are external inputs.
They come from the user's own device firmware and are not part of this
repository.

## Host requirements

- a POSIX host with Git, Python 3 and a conforming C toolchain;
- enough space for the Mu, EDK II and OpenSSL source trees;
- the submodules declared in `.gitmodules`.

## 1. Populate source dependencies

```sh
git submodule update --init --recursive
```

This checks out the exact upstream commits recorded by the repository.

## 2. Populate PHK110 vendor inputs

Extract the PHK110 UEFI modules and raw files into:

```text
Binaries/udon/
```

The Udon package expects the Qualcomm and OnePlus modules referenced by
`udonPkg/Include/APRIORI.inc`, `DXE.inc`, `RAW.inc` and `udon.fdf`. At minimum
this includes the UFS, USB device, display, PMIC, TLMM, SPMI, GLink, clock and
platform information modules. The correct source is the ABL/UEFI firmware of
the target PHK110, not another SM8475 phone.

This directory is intentionally absent from public Git and from CI.

## 3. Build

```sh
export UNIXNU_ROOT=/path/to/UniXNU
./scripts/build-udon-firmware.sh
```

The script checks the UniXNU package and `Binaries/udon`, builds BaseTools if
needed, and invokes EDK II with:

```text
FD_BASE=0xA7100000
FD_SIZE=0x400000
FD_BLOCKS=0x400
AARCH64 DEBUG CLANGPDB
```

Expected output:

```text
Build/udonPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd
```

The working local build produced a 4 MiB FD with SHA-256
`08e527052dcb57510d040141d717d4a8ea234c7b77ab3174e4920fa9c5db2561`.
Different compilers or submodule revisions can change the hash.

## 4. Static checks

```sh
python3 scripts/check-udon-platform.py
```

CI runs the same check. It validates the device files, FD layout, DSC/FDF
entry points and shell syntax without requiring the proprietary modules.

## 5. Flashing

The stock PHK110 ABL uses an OEM AVB footer/graft path. Follow the companion
`UniXNU-udon` repository's handoff record and keep stock partition backups.
Do not flash this firmware on another device.
