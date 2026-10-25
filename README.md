# uefi-udon

> **Work in progress.** UEFI, UniXNU, DarwinBoot and the target XNU kernel run on
> the OnePlus Ace 2 / 11R (PHK110, Udon, SM8475).

UEFI platform integration for the OnePlus Ace 2 / 11R, based on
[`UniXNU/uefi-sheng`](https://github.com/UniXNU/uefi-sheng) and the
Mu-Silicium tree. The companion device repository is
[`UniXNU-udon`](../unixnu/UniXNU-udon).

## Current state

| Area | State | Notes |
| --- | --- | --- |
| Firmware build | ✅ | 4 MiB DEBUG FD at `0xA7100000` |
| UEFI on PHK110 | ✅ | Booted through the `boot_b` AVB/OEM-footer graft path |
| Display handoff | ✅ | `SimpleFbDxe`, 1240 × 2772 × 32-bit |
| UniXNU menu | ✅ | Visible on hardware |
| DarwinBoot and XNU handoff | ✅ | XNU and its diagnostics execute |
| Payload volume | ✅ | FAT payload is loaded from `super` |
| Udon persistent log export | ❌ | The UEFI app runs, but host-side USB mass-storage access is not qualified |
| Linux `ReadyToBoot` boot | ⚠️ | Diagnostic driver is present; not the current boot path |
| Touch, USB host, sleep, audio | ❌ | Not available in this milestone |

The XNU bring-up status is tracked in
[`UniXNU-udon/docs/STATUS.md`](../unixnu/UniXNU-udon/docs/STATUS.md).
The UEFI milestone record is in [`docs/STATUS.md`](docs/STATUS.md).

## What is Udon-specific

| Path | Purpose |
| --- | --- |
| `Platforms/OnePlus/udonPkg/` | PHK110 platform, memory map, configuration map, firmware inputs, UniXNU integration |
| `Platforms/OnePlus/udonPkg/Drivers/ShengBootMemoryDxe` | Publishes DRAM and the boot-image RAM disk from the FDT |
| `Platforms/OnePlus/udonPkg/Applications/UdonLogExport` | Experimental read-only RAM export of the final 32 MiB of userdata |
| `Platforms/OnePlus/udonPkg/LinuxBoot` | Experimental `ReadyToBoot` GPT kernel launcher |
| `Resources/Configs/udon.toml` | FD base/size and Android boot-image layout |

The package uses vendor modules from `Binaries/udon/` in the
`Project-Silicium/Device-Binaries` submodule. Those binaries are proprietary,
are not included here, and must come from the user's own device firmware dump.

## Build

Host prerequisites and upstream integration are documented in
[`Building.md`](Building.md). The exact reproduction path, including the
external PHK110 vendor input, is in [`docs/REPRODUCE.md`](docs/REPRODUCE.md).
Initialize the submodules first:

```sh
git submodule update --init --recursive
export UNIXNU_ROOT=/path/to/UniXNU
./scripts/build-udon-firmware.sh
```

The built firmware is:

```text
Build/udonPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd
```

Run the repository checks with:

```sh
python3 scripts/check-udon-platform.py
```

The common UniXNU flow can also invoke this tree as the firmware repository:

```sh
./unixnu build package firmware --uefi /path/to/uefi-udon \
  -D /path/to/UniXNU-udon
```

`FD_BASE`, `FD_SIZE` and `FD_BLOCKS` must stay consistent with
`Resources/Configs/udon.toml` and `udonPkg/Library/MemoryMapLib`.

## Flashing notes

- Android slot `b` was used during bring-up.
- The stock PHK110 ABL requires the boot image's OEM vbmeta/footer graft.
- `boot_b`, `vendor_boot_b`, `dtbo_b` and `vbmeta_b` should be backed up first.
- The payload lives in the FAT volume in `super` for the current XNU path.

## Known cleanup items

- The inherited DSC still carries Realme SMBIOS strings from the `senna_bc`
  template.
- `LinuxBootDxe` and `UdonLogExport` are experiments, not supported boot paths.
- The vendor binary directory is not reproducible from public sources alone.

## Credits

Project Mu, Project Silicium, WOA-Project, Project Aloha and the UniXNU
contributors provide the base firmware, platform libraries and documentation.

## License

All code except `GPLDrivers`, `GPLApplications` and `GPLLibrary` is licensed
under BSD-2-Clause. GPL components are licensed under GPLv2 or later. See
[`LICENSE`](LICENSE).
