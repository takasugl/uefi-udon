#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
UNIXNU_ROOT=${UNIXNU_ROOT:-"$ROOT/../unixnu/UniXNU"}
BASECORE="$ROOT/Mu_Basecore"
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}

if [[ ! -d "$UNIXNU_ROOT/package/UniXNUPkg" ]]; then
  echo "UniXNU root not found at $UNIXNU_ROOT; set UNIXNU_ROOT" >&2
  exit 1
fi
if [[ ! -d "$ROOT/Binaries/udon" ]]; then
  echo "Binaries/udon is missing; populate the Device-Binaries submodule with the PHK110 dump" >&2
  exit 1
fi

if [[ ! -x "$BASECORE/BaseTools/Source/C/bin/GenFw" ]]; then
  make -C "$BASECORE/BaseTools/Source/C" -j"$JOBS"
fi

export WORKSPACE="$ROOT"
export EDK_TOOLS_PATH="$BASECORE/BaseTools"
export PACKAGES_PATH="$ROOT:$ROOT/Platforms/OnePlus:$UNIXNU_ROOT/package:$UNIXNU_ROOT/drivers/DarwinBoot:$BASECORE:$ROOT/Common/Mu:$ROOT/Common/Mu_OEM_Sample:$ROOT/Silicon/Qualcomm:$ROOT/Silicon/Silicium:$ROOT/Silicium-ACPI:$ROOT/Silicium-ACPI/Platforms/OnePlus:$ROOT/Silicium-ACPI/Silicon/Qualcomm"
export PYTHON_COMMAND=${PYTHON_COMMAND:-python3}
export PATH="$BASECORE/BaseTools/BinWrappers/PosixLike:$BASECORE/BaseTools/Source/C/bin:$PATH"

BUILD_ROOT=${BUILD_ROOT:-"$ROOT/Build"}
CONF_DIR=${CONF_DIR:-"$BUILD_ROOT/Conf"}
mkdir -p "$CONF_DIR"
for name in target tools_def build_rule; do
  if [[ ! -f "$CONF_DIR/$name.txt" ]]; then
    cp "$BASECORE/BaseTools/Conf/$name.template" "$CONF_DIR/$name.txt"
  fi
done
export CONF_PATH="$CONF_DIR"

cd "$ROOT"
build \
  -p Platforms/OnePlus/udonPkg/udon.dsc \
  -a AARCH64 \
  -b DEBUG \
  -t CLANGPDB \
  -n "$JOBS" \
  -D FD_BASE=0xA7100000 \
  -D FD_SIZE=0x400000 \
  -D FD_BLOCKS=0x400 \
  -D DEVICE_MODEL=0 \
  -D ENABLE_SECUREBOOT=0 \
  -D MEMORY_PROTECTION=TRUE \
  -D SHIP_MODE=FALSE \
  -D BUILDID_STRING=udon-research

echo "firmware: $BUILD_ROOT/udonPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd"
