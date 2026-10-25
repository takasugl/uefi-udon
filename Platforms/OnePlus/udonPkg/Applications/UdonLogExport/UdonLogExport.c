/** @file
  Export the persistent Udon XNU log window as a read-only USB mass-storage disk.

  The BootLog kernel kit stores an 8192-block, 4 KiB-per-block SHLOG001 window at
  the end of the GPT partition named "userdata". This application copies exactly
  that 32 MiB window into RAM and assigns the RAM block device to USB LUN 0.
  Keeping UFS behind the RAM copy avoids relying on macOS accepting the vendor
  UFS capacity/SCSI behavior directly.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/EFIUsbMsd.h>

#define LOG_WINDOW_BYTES   SIZE_32MB
#define RAM_BLOCK_SIZE     512
#define COPY_CHUNK_BYTES   SIZE_128KB

STATIC EFI_BLOCK_IO_MEDIA  mRamMedia;
STATIC EFI_BLOCK_IO_PROTOCOL  mRamBlockIo;
STATIC EFI_HANDLE          mRamHandle;
STATIC UINT8               *mRamBase;

STATIC
BOOLEAN
UdonStriCmpEqual (
  IN CONST CHAR16  *A,
  IN CONST CHAR16  *B
  )
{
  while ((*A != 0) && (*B != 0)) {
    if ((*A | 0x20) != (*B | 0x20)) {
      return FALSE;
    }

    A++;
    B++;
  }

  return (*A == 0) && (*B == 0);
}

STATIC
EFI_STATUS
EFIAPI
RamBlockReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  UINT64  Start;
  UINT64  End;

  if ((This == NULL) || (This->Media == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (MediaId != This->Media->MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if ((BufferSize % This->Media->BlockSize) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Start = MultU64x32 (Lba, This->Media->BlockSize);
  End   = Start + BufferSize;
  if ((End < Start) || (End > (UINT64)(This->Media->LastBlock + 1) * This->Media->BlockSize)) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, mRamBase + Start, BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
CopyLogWindow (
  IN  EFI_BLOCK_IO_PROTOCOL  *Disk,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                FirstLba,
  IN  EFI_LBA                LastLba,
  IN  UINTN                  BlockSize,
  OUT UINT8                  **Window
  )
{
  EFI_STATUS  Status;
  UINT64      Blocks;
  UINT64      StartLba;
  UINTN       Offset;
  UINTN       ChunkBytes;
  UINT8       *Buffer;

  if ((BlockSize == 0) || ((LOG_WINDOW_BYTES % BlockSize) != 0)) {
    Print (L"[udon-log] unsupported source block size %u\n", (UINT32)BlockSize);
    return EFI_UNSUPPORTED;
  }

  Blocks = LOG_WINDOW_BYTES / BlockSize;
  if ((LastLba < FirstLba) || ((LastLba + 1 - FirstLba) < Blocks)) {
    Print (L"[udon-log] userdata is smaller than the 32 MiB log window\n");
    return EFI_NOT_FOUND;
  }

  StartLba = LastLba + 1 - Blocks;
  Buffer   = AllocatePool (LOG_WINDOW_BYTES);
  if (Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ChunkBytes  = MIN (COPY_CHUNK_BYTES, LOG_WINDOW_BYTES);
  ChunkBytes -= ChunkBytes % BlockSize;
  for (Offset = 0; Offset < LOG_WINDOW_BYTES; Offset += ChunkBytes) {
    UINTN ThisChunk = MIN (ChunkBytes, LOG_WINDOW_BYTES - Offset);
    Status = Disk->ReadBlocks (
                     Disk,
                     MediaId,
                     StartLba + Offset / BlockSize,
                     ThisChunk,
                     Buffer + Offset
                     );
    if (EFI_ERROR (Status)) {
      Print (L"[udon-log] userdata read failed at offset %u: %r\n", (UINT32)Offset, Status);
      FreePool (Buffer);
      return Status;
    }
  }

  *Window = Buffer;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
FindUserdata (
  OUT EFI_BLOCK_IO_PROTOCOL  **Disk,
  OUT UINT32                 *MediaId,
  OUT EFI_LBA                *FirstLba,
  OUT EFI_LBA                *LastLba
  )
{
  EFI_STATUS              Status;
  UINTN                   HandleCount;
  EFI_HANDLE              *Handles;
  UINTN                   Index;
  EFI_BLOCK_IO_PROTOCOL   *BlockIo;
  UINT8                   *Header;
  UINT64                  EntryLba;
  UINT32                  EntryCount;
  UINT32                  EntrySize;
  UINTN                   EntriesBytes;
  UINTN                   EntriesReadBytes;
  UINT8                   *Entries;
  UINT32                  EntryIndex;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (EFI_ERROR (Status) || (BlockIo->Media == NULL) ||
        !BlockIo->Media->MediaPresent || BlockIo->Media->LogicalPartition ||
        (BlockIo->Media->BlockSize < 512))
    {
      continue;
    }

    Header = AllocateZeroPool (BlockIo->Media->BlockSize);
    if (Header == NULL) {
      continue;
    }

    Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, 1, BlockIo->Media->BlockSize, Header);
    if (EFI_ERROR (Status) || (CompareMem (Header, "EFI PART", 8) != 0)) {
      FreePool (Header);
      continue;
    }

    EntryLba   = ReadUnaligned64 (Header + 72);
    EntryCount = ReadUnaligned32 (Header + 80);
    EntrySize  = ReadUnaligned32 (Header + 84);
    FreePool (Header);
    if ((EntryCount == 0) || (EntryCount > 128) || (EntrySize < 128) || (EntrySize > 1024)) {
      continue;
    }

    EntriesBytes     = (UINTN)EntryCount * EntrySize;
    EntriesReadBytes = ALIGN_VALUE (EntriesBytes, BlockIo->Media->BlockSize);
    Entries          = AllocateZeroPool (EntriesReadBytes);
    if (Entries == NULL) {
      continue;
    }

    Status = BlockIo->ReadBlocks (
                        BlockIo,
                        BlockIo->Media->MediaId,
                        EntryLba,
                        EntriesReadBytes,
                        Entries
                        );
    if (EFI_ERROR (Status)) {
      FreePool (Entries);
      continue;
    }

    for (EntryIndex = 0; EntryIndex < EntryCount; EntryIndex++) {
      UINT8   *Entry = Entries + (UINTN)EntryIndex * EntrySize;
      CHAR16  Name[36];

      if (IsZeroBuffer (Entry, 16)) {
        continue;
      }

      CopyMem (Name, Entry + 56, 72);
      Name[35] = 0;
      if (!UdonStriCmpEqual (Name, L"userdata")) {
        continue;
      }

      *Disk    = BlockIo;
      *MediaId = BlockIo->Media->MediaId;
      *FirstLba = ReadUnaligned64 (Entry + 32);
      *LastLba  = ReadUnaligned64 (Entry + 40);
      FreePool (Entries);
      FreePool (Handles);
      return EFI_SUCCESS;
    }

    FreePool (Entries);
  }

  FreePool (Handles);
  return EFI_NOT_FOUND;
}

STATIC
VOID
InitRamBlock (
  IN UINT8  *Window
  )
{
  ZeroMem (&mRamMedia, sizeof (mRamMedia));
  mRamBase                   = Window;
  mRamMedia.MediaId          = 1;
  mRamMedia.RemovableMedia   = FALSE;
  mRamMedia.MediaPresent     = TRUE;
  mRamMedia.LogicalPartition = FALSE;
  mRamMedia.ReadOnly         = TRUE;
  mRamMedia.WriteCaching     = FALSE;
  mRamMedia.BlockSize        = RAM_BLOCK_SIZE;
  mRamMedia.IoAlign          = 1;
  mRamMedia.LastBlock        = LOG_WINDOW_BYTES / RAM_BLOCK_SIZE - 1;

  mRamBlockIo.Revision       = EFI_BLOCK_IO_PROTOCOL_REVISION;
  mRamBlockIo.Media          = &mRamMedia;
  mRamBlockIo.Reset          = RamBlockReset;
  mRamBlockIo.ReadBlocks     = RamBlockRead;
  mRamBlockIo.WriteBlocks    = RamBlockWrite;
  mRamBlockIo.FlushBlocks    = RamBlockFlush;
}

EFI_STATUS
EFIAPI
UdonLogExportEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS             Status;
  EFI_BLOCK_IO_PROTOCOL  *Disk;
  UINT32                 SourceMediaId;
  EFI_LBA                FirstLba;
  EFI_LBA                LastLba;
  UINT8                  *Window;
  EFI_USB_MSD_PROTOCOL   *Msd;
  CHAR8                  Magic[9];

  Print (L"\r\n[udon-log] reading persistent SHLOG001 window...\r\n");
  Status = FindUserdata (&Disk, &SourceMediaId, &FirstLba, &LastLba);
  if (EFI_ERROR (Status)) {
    Print (L"[udon-log] userdata GPT partition not found: %r\r\n", Status);
    return Status;
  }

  Status = CopyLogWindow (Disk, SourceMediaId, FirstLba, LastLba, Disk->Media->BlockSize, &Window);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CopyMem (Magic, Window, 8);
  Magic[8] = 0;
  Print (L"[udon-log] copied 32 MiB userdata tail; magic=%a\r\n", Magic);

  InitRamBlock (Window);
  mRamHandle = NULL;
  Status = gBS->InstallProtocolInterface (
                  &mRamHandle,
                  &gEfiBlockIoProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mRamBlockIo
                  );
  if (EFI_ERROR (Status)) {
    FreePool (Window);
    return Status;
  }

  Status = gBS->LocateProtocol (&gEfiUsbMsdProtocolGuid, NULL, (VOID **)&Msd);
  if (EFI_ERROR (Status)) {
    Print (L"[udon-log] USB mass-storage protocol unavailable: %r\r\n", Status);
    return Status;
  }

  Status = Msd->AssignBlkIoHandle (Msd, &mRamBlockIo, 0);
  if (EFI_ERROR (Status)) {
    Print (L"[udon-log] assigning RAM disk to USB LUN 0 failed: %r\r\n", Status);
    return Status;
  }

  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  Status = Msd->StartDevice (Msd);
  if (EFI_ERROR (Status)) {
    Msd->AssignBlkIoHandle (Msd, NULL, 0);
    Print (L"[udon-log] starting USB device failed: %r\r\n", Status);
    return Status;
  }

  Print (L"[udon-log] USB LUN0 is the 32 MiB log window; [Volume Up] exits\r\n");
  for (;;) {
    EFI_INPUT_KEY  Key;

    Msd->EventHandler (Msd);
    if (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key)) && (Key.ScanCode == SCAN_UP)) {
      break;
    }

    gBS->Stall (10000);
  }

  Msd->StopDevice (Msd);
  Msd->AssignBlkIoHandle (Msd, NULL, 0);
  return EFI_SUCCESS;
}
