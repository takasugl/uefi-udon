/** @file
  udon Linux Boot Dxe

  在 ReadyToBoot 时：
    1. 在屏幕上打印移交诊断（FDT 配置表 / GOP 模式 / GPT 分区扫描结果）；
    2. 若 UFS 上存在指定分区且其头部是合法 PE 镜像（内核 zboot vmlinuz.efi），
       则读入内存、附带命令行并 StartImage 直接引导 Linux；
    3. 否则什么也不做，BDS 继续常规流程（UEFI Shell 等）。

  内核写入方式（Android 侧，root）：
    dd if=vmlinuz.efi of=/dev/block/by-name/logfs bs=4M
  分区名通过 PARTITION_NAME 修改；命令行可用 UEFI 变量
  "UdonLinuxCmdline"（ASCII）覆盖默认值。
**/

#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Guid/Fdt.h>
#include <IndustryStandard/PeImage.h>
#include <Library/PrintLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>

//
// 配置：内核所在 GPT 分区名（UTF-16）与默认内核命令行
//
#define UDON_KERNEL_PARTITION  L"logdump"
#define UDON_CMDLINE_VARIABLE  L"UdonLinuxCmdline"
#define UDON_DEFAULT_CMDLINE   L"console=tty0 ignore_loglevel loglevel=8 panic=0"

#define UDON_PE_HEADER_READ_SIZE  SIZE_64KB
#define UDON_MAX_KERNEL_SIZE      SIZE_256MB

STATIC BOOLEAN mAttemptedBoot = FALSE;

STATIC
VOID
UdonPrint (
  IN CONST CHAR16 *Format,
  ...
  )
{
  VA_LIST  Args;
  CHAR16   Buffer[256];

  VA_START (Args, Format);
  UnicodeVSPrint (Buffer, sizeof (Buffer), Format, Args);
  VA_END (Args);

  if (gST != NULL && gST->ConOut != NULL) {
    gST->ConOut->OutputString (gST->ConOut, Buffer);
  }

  DEBUG ((DEBUG_INFO, "%s", Buffer));
}

/**
  在系统配置表中查找 FDT 表并打印概况。
**/
STATIC
VOID
PrintDiagnostics (
  VOID
  )
{
  EFI_STATUS                       Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL    *Gop;
  EFI_CONFIGURATION_TABLE         *Entry;
  VOID                            *Fdt;
  UINTN                            Index;

  Fdt = NULL;
  for (Index = 0; Index < gST->NumberOfTableEntries; Index++) {
    Entry = &gST->ConfigurationTable[Index];
    if (CompareGuid (&Entry->VendorGuid, &gFdtTableGuid)) {
      Fdt = Entry->VendorTable;
      break;
    }
  }

  if (Fdt != NULL && *(UINT32 *)Fdt == 0xD00DFEED) {
    UdonPrint (L"[udon-linux] FDT config table: present (magic OK)\r\n");
  } else {
    UdonPrint (L"[udon-linux] FDT config table: MISSING! (DtPlatformDxe/variable store problem)\r\n");
  }

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (!EFI_ERROR (Status) && Gop->Mode != NULL) {
    UdonPrint (
      L"[udon-linux] GOP: %dx%d fb@0x%lx\r\n",
      Gop->Mode->Info->HorizontalResolution,
      Gop->Mode->Info->VerticalResolution,
      Gop->Mode->FrameBufferBase
      );
  } else {
    UdonPrint (L"[udon-linux] GOP: not available\r\n");
  }
}

/**
  Unicode 大小写不敏感比较（MdePkg 无 StriCmp，自己实现）。
**/
STATIC
BOOLEAN
UdonStriCmpEqual (
  IN CONST CHAR16 *A,
  IN CONST CHAR16 *B
  )
{
  while (*A != 0 && *B != 0) {
    if ((*A | 0x20) != (*B | 0x20)) {
      return FALSE;
    }
    A++;
    B++;
  }

  return (*A == 0) && (*B == 0);
}

/**
  扫描所有 BlockIo，读取 LBA1 找 GPT 头，按名字查找分区。

  @param[out] DiskBlockIo   整盘 BlockIo
  @param[out] MediaId       整盘 MediaId
  @param[out] FirstLba      分区起始 LBA
  @param[out] PartitionSize 分区字节数

  @retval EFI_SUCCESS 找到目标分区
**/
STATIC
EFI_STATUS
FindKernelPartition (
  OUT EFI_BLOCK_IO_PROTOCOL **DiskBlockIo,
  OUT UINT32                 *MediaId,
  OUT EFI_LBA                *FirstLba,
  OUT UINT64                 *PartitionSize
  )
{
  EFI_STATUS              Status;
  UINTN                   HandleCount;
  EFI_HANDLE              *Handles;
  UINTN                   Index;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  UINT8                   Header[512];
  UINT64                  EntryLba;
  UINT32                  EntryCount;
  UINT32                  EntrySize;
  UINT8                  *Entries;
  UINTN                   EntriesSize;
  UINT32                  i;
  UINT8                  *Entry;
  CHAR16                  Name[36];

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (EFI_ERROR (Status) || BlockIo->Media == NULL || !BlockIo->Media->MediaPresent) {
      continue;
    }

    // 只在整盘上找 GPT（分区 BlockIo 的 LBA1 不会有 "EFI PART"）
    SetMem (Header, sizeof (Header), 0);
    Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, 1, sizeof (Header), Header);
    if (EFI_ERROR (Status) ||
        CompareMem (Header, "EFI PART", 8) != 0)
    {
      continue;
    }

    EntryLba   = *(UINT64 *)(Header + 72);
    EntryCount = *(UINT32 *)(Header + 80);
    EntrySize  = *(UINT32 *)(Header + 84);

    if (EntryCount == 0 || EntryCount > 128 || EntrySize < 128 || EntrySize > 1024) {
      continue;
    }

    EntriesSize = (UINTN)EntryCount * EntrySize;
    Entries = AllocatePool (EntriesSize);
    if (Entries == NULL) {
      continue;
    }

    Status = BlockIo->ReadBlocks (
                        BlockIo,
                        BlockIo->Media->MediaId,
                        EntryLba,
                        EntriesSize,
                        Entries
                        );
    if (EFI_ERROR (Status)) {
      FreePool (Entries);
      continue;
    }

    for (i = 0; i < EntryCount; i++) {
      Entry = Entries + (UINTN)i * EntrySize;

      // Type GUID 全零 = 未使用
      if (IsZeroBuffer (Entry, 16)) {
        continue;
      }

      // 分区名：UTF-16LE，最多 72 字节
      CopyMem (Name, Entry + 56, 72);
      Name[35] = 0;

      if (!UdonStriCmpEqual (Name, UDON_KERNEL_PARTITION)) {
        continue;
      }

      *DiskBlockIo   = BlockIo;
      *MediaId       = BlockIo->Media->MediaId;
      *FirstLba      = *(UINT64 *)(Entry + 32);
      *PartitionSize = (*(UINT64 *)(Entry + 40) - *FirstLba + 1) * BlockIo->Media->BlockSize;

      UdonPrint (
        L"[udon-linux] kernel partition \"%s\": %d MB\r\n",
        Name,
        (UINT32)(*PartitionSize / (1024 * 1024))
        );

      FreePool (Entries);
      gBS->FreePool (Handles);
      return EFI_SUCCESS;
    }

    FreePool (Entries);
  }

  gBS->FreePool (Handles);
  return EFI_NOT_FOUND;
}

/**
  校验缓冲区头部是 PE32+ 镜像并返回 SizeOfImage。
**/
STATIC
EFI_STATUS
GetPeImageSize (
  IN  UINT8  *Head,
  IN  UINTN  HeadSize,
  OUT UINT64 *ImageSize
  )
{
  EFI_IMAGE_DOS_HEADER             *DosHeader;
  UINT32                           PeOffset;
  EFI_IMAGE_OPTIONAL_HEADER_UNION  *PeHeader;

  if (HeadSize < UDON_PE_HEADER_READ_SIZE) {
    return EFI_INVALID_PARAMETER;
  }

  DosHeader = (EFI_IMAGE_DOS_HEADER *)Head;
  if (DosHeader->e_magic != EFI_IMAGE_DOS_SIGNATURE) {
    return EFI_UNSUPPORTED;
  }

  PeOffset = DosHeader->e_lfanew;
  if (PeOffset == 0 || PeOffset > HeadSize - 256) {
    return EFI_UNSUPPORTED;
  }

  PeHeader = (EFI_IMAGE_OPTIONAL_HEADER_UNION *)(Head + PeOffset);
  if (PeHeader->Pe32.Signature != EFI_IMAGE_NT_SIGNATURE) {
    return EFI_UNSUPPORTED;
  }

  if (PeHeader->Pe32.OptionalHeader.Magic != EFI_IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    return EFI_UNSUPPORTED;
  }

  *ImageSize = PeHeader->Pe32Plus.OptionalHeader.SizeOfImage;
  if (*ImageSize < SIZE_1MB || *ImageSize > UDON_MAX_KERNEL_SIZE) {
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

/**
  ReadyToBoot 回调：诊断 + 尝试引导内核。
**/
STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS               Status;
  EFI_BLOCK_IO_PROTOCOL   *DiskBlockIo;
  UINT32                   MediaId;
  EFI_LBA                  FirstLba;
  UINT64                   PartitionSize;
  EFI_PHYSICAL_ADDRESS     Buffer;
  UINTN                    Pages;
  UINT64                   ImageSize;
  UINTN                    ReadSize;
  UINTN                    Offset;
  EFI_HANDLE               KernelHandle;
  EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
  CHAR16                  *Cmdline;
  UINTN                    ExitDataSize;
  CHAR16                  *ExitData;

  if (mAttemptedBoot) {
    return;
  }
  mAttemptedBoot = TRUE;

  UdonPrint (L"\r\n[udon-linux] ReadyToBoot diagnostics\r\n");
  PrintDiagnostics ();

  Status = FindKernelPartition (&DiskBlockIo, &MediaId, &FirstLba, &PartitionSize);
  if (EFI_ERROR (Status)) {
    UdonPrint (L"[udon-linux] no \"%s\" partition with kernel, continuing normal boot\r\n", UDON_KERNEL_PARTITION);
    return;
  }

  // 先读头部，解析 SizeOfImage
  Buffer = 0;
  Status = gBS->AllocatePages (AllocateAnyPages, EfiBootServicesData, EFI_SIZE_TO_PAGES (UDON_PE_HEADER_READ_SIZE), &Buffer);
  if (EFI_ERROR (Status)) {
    UdonPrint (L"[udon-linux] alloc head buffer failed: %r\r\n", Status);
    return;
  }

  Status = DiskBlockIo->ReadBlocks (DiskBlockIo, MediaId, FirstLba, UDON_PE_HEADER_READ_SIZE, (VOID *)(UINTN)Buffer);
  if (EFI_ERROR (Status)) {
    UdonPrint (L"[udon-linux] read head failed: %r\r\n", Status);
    gBS->FreePages (Buffer, EFI_SIZE_TO_PAGES (UDON_PE_HEADER_READ_SIZE));
    return;
  }

  Status = GetPeImageSize ((UINT8 *)(UINTN)Buffer, UDON_PE_HEADER_READ_SIZE, &ImageSize);
  gBS->FreePages (Buffer, EFI_SIZE_TO_PAGES (UDON_PE_HEADER_READ_SIZE));
  if (EFI_ERROR (Status)) {
    UdonPrint (L"[udon-linux] partition head is not a kernel PE image (%r), continuing normal boot\r\n", Status);
    return;
  }

  if (ImageSize > PartitionSize) {
    UdonPrint (L"[udon-linux] kernel larger than partition! img=%d MB\r\n", (UINT32)(ImageSize / (1024 * 1024)));
    return;
  }

  // 读入整个内核
  Pages = EFI_SIZE_TO_PAGES ((UINTN)ImageSize);
  Buffer = 0;
  Status = gBS->AllocatePages (AllocateAnyPages, EfiBootServicesData, Pages, &Buffer);
  if (EFI_ERROR (Status)) {
    UdonPrint (L"[udon-linux] alloc kernel buffer (%d MB) failed: %r\r\n", (UINT32)(ImageSize / (1024 * 1024)), Status);
    return;
  }

  Offset = 0;
  while (Offset < ImageSize) {
    ReadSize = MIN ((UINTN)(ImageSize - Offset), SIZE_1MB);
    Status   = DiskBlockIo->ReadBlocks (
                              DiskBlockIo,
                              MediaId,
                              FirstLba + (EFI_LBA)(Offset / DiskBlockIo->Media->BlockSize),
                              ReadSize,
                              (VOID *)(UINTN)(Buffer + Offset)
                              );
    if (EFI_ERROR (Status)) {
      UdonPrint (L"[udon-linux] read kernel chunk failed: %r\r\n", Status);
      gBS->FreePages (Buffer, Pages);
      return;
    }
    Offset += ReadSize;
  }

  UdonPrint (L"[udon-linux] kernel loaded (%d MB), starting...\r\n", (UINT32)(ImageSize / (1024 * 1024)));

  Status = gBS->LoadImage (FALSE, gImageHandle, NULL, (VOID *)(UINTN)Buffer, (UINTN)ImageSize, &KernelHandle);
  if (EFI_ERROR (Status)) {
    UdonPrint (L"[udon-linux] LoadImage failed: %r\r\n", Status);
    gBS->FreePages (Buffer, Pages);
    return;
  }

  // 命令行：UEFI 变量可覆盖默认值（ASCII 存，UTF-16 用）
  Cmdline = NULL;
  Status  = GetEfiGlobalVariable2 (UDON_CMDLINE_VARIABLE, (VOID **)&Cmdline, NULL);
  if (EFI_ERROR (Status) || Cmdline == NULL) {
    Cmdline = AllocateCopyPool (sizeof (UDON_DEFAULT_CMDLINE), UDON_DEFAULT_CMDLINE);
  }

  Status = gBS->HandleProtocol (KernelHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&LoadedImage);
  if (!EFI_ERROR (Status)) {
    LoadedImage->LoadOptions     = Cmdline;
    LoadedImage->LoadOptionsSize = (UINT32)StrSize (Cmdline);
  }

  UdonPrint (L"[udon-linux] StartImage, bye UEFI...\r\n");
  Status = gBS->StartImage (KernelHandle, &ExitDataSize, &ExitData);
  if (EFI_ERROR (Status)) {
    UdonPrint (L"[udon-linux] kernel returned: %r\r\n", Status);
    gBS->UnloadImage (KernelHandle);
    gBS->FreePages (Buffer, Pages);
  }
}

/**
  驱动入口：注册 ReadyToBoot 事件。
**/
EFI_STATUS
EFIAPI
LinuxBootDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;
  EFI_EVENT  ReadyToBootEvent;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnReadyToBoot,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &ReadyToBootEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[udon-linux] failed to create ReadyToBoot event: %r\n", Status));
    return Status;
  }

  return EFI_SUCCESS;
}
