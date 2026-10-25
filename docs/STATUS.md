# UEFI / PHK110 status

Status date: 2026-10-25

## Hardware milestones

1. A Mu-Silicium build booted on the OnePlus Ace 2 and displayed the UEFI menu.
2. A Udon-specific `udonPkg` derived from the SM8475 `senna_bc` platform built
   successfully with UniXNU, `UniXNUDxe` and DarwinBoot.
3. The UniXNU interface appeared on the PHK110 display.
4. The current boot chain is:

   ```text
   ABL -> boot_b (Mu-Silicium UEFI + UniXNU + DarwinBoot)
       -> FAT payload in super -> XNU
   ```

5. XNU reaches launchd and a live userspace diagnostic page. That progress is
   tracked in the companion device repository rather than inferred from the UEFI
   boot alone.

## Firmware properties

| Property | Value |
| --- | --- |
| SoC | SM8475 |
| FD base | `0xA7100000` |
| FD size | `0x400000` |
| Build target | AARCH64, DEBUG, CLANGPDB |
| Display | 1240 × 2772, 32-bit, firmware/ABL splash buffer |
| Payload volume | 96 MiB FAT volume in `super` |

## Experiments

### UEFI Mass Storage

The UEFI mass-storage app enumerated as
`QUALCOMM USB MASS STORAGE` (`05c6:f000`) and exposed SCSI LUNs, but macOS did not
create an `IOMedia` or `/dev/disk*` node.

### RAM log export

`UdonLogExport.efi` copies the final 32 MiB of `userdata` into RAM, validates
`SHLOG001`, and publishes only that RAM block. The device enumerated, but the
host still stalled before creating an `IOMedia`.

A direct USB Bulk-Only Transport reader bypassed the macOS storage stack. It
claimed the interface but timed out on the first CBW write to endpoint `0x01`.
This path is deferred.

### Linux ReadyToBoot

`udonPkg/LinuxBoot` is a diagnostic and launcher experiment for a Linux PE on a
GPT partition. It is not used by the current XNU boot path.

## Open work

- Revisit the persistent-log transport only after the current XNU userspace
  investigation requires it.
- Replace inherited SMBIOS strings and remove unused template content before
  calling the platform production-ready.
- Keep the proprietary binary requirement explicit when publishing builds.
