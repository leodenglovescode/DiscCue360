#include "drive.h"

#include <stdio.h>
#include <string.h>
#include <xsdk/log.h>

enum {
    READ_TOC = 0x24000,
    READ_TOC_EX = 0x24054,
    CHECK_MEDIA = 0x24800,
    SCSI_PASS_THROUGH_DIRECT = 0x4d014,
    SCSI_READ_TOC = 0x43,
    SCSI_READ_CD = 0xbe,
    SCSI_DATA_IN = 1,
};
static const NTSTATUS DATA_OVERRUN = (NTSTATUS)0xc000003c;

// The 360's SCSI pass-through request has a 32-bit pointer and no sense-buffer offset.
typedef struct {
    uint16_t length;
    uint8_t scsi_status;
    uint8_t path_id;
    uint8_t target_id;
    uint8_t lun;
    uint8_t cdb_length;
    uint8_t direction;
    uint32_t transfer_bytes;
    uint32_t timeout_seconds;
    uint8_t *data;
    uint8_t cdb[16];
} ScsiRead;

#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(ScsiRead) == 36, "Xbox 360 SCSI request ABI");
#endif

static bool decode_toc(uint32_t code, NTSTATUS status, uint32_t size, const uint8_t *bytes, DiscToc *disc)
{
    if (!NT_SUCCESS(status) && status != DATA_OVERRUN)
        size = 0;
    xsdk_log_write("READ TOC 0x%05X: status=0x%08X bytes=%u head=%02X %02X %02X %02X",
        (unsigned)code, (unsigned)status, (unsigned)size, bytes[0], bytes[1], bytes[2], bytes[3]);
    const bool delivered = NT_SUCCESS(status) ||
        (code == SCSI_PASS_THROUGH_DIRECT && status == DATA_OVERRUN);
    return delivered && size <= CD_TOC_BYTES && parse_toc(bytes, size, disc);
}

static NTSTATUS scsi_read_toc(uint32_t device, uint8_t *bytes, uint32_t allocation,
    uint8_t *scsi_status, uint32_t *transferred)
{
    ScsiRead request = {
        .length = sizeof(ScsiRead), .cdb_length = 10, .direction = SCSI_DATA_IN,
        .transfer_bytes = allocation, .timeout_seconds = 4, .data = bytes,
        .cdb = { SCSI_READ_TOC, 2, 0, 0, 0, 0, 0, (uint8_t)(allocation >> 8), (uint8_t)allocation, 0 }
    };
    memset(bytes, 0, CD_TOC_BYTES);
    const NTSTATUS status = IoSynchronousDeviceIoControlRequest(
        SCSI_PASS_THROUGH_DIRECT, device, &request, sizeof(request), &request, sizeof(request), NULL);
    *scsi_status = request.scsi_status;
    *transferred = request.transfer_bytes <= allocation ? request.transfer_bytes : 0;
    xsdk_log_write("SCSI READ TOC: requested=%u scsi=0x%02X", (unsigned)allocation, (unsigned)*scsi_status);
    return status;
}

NTSTATUS open_optical(uint32_t *device)
{
    static const char path[] = "\\Device\\Cdrom0";
    const ANSI_STRING name = { sizeof(path) - 1, sizeof(path), path };
    *device = 0;
    const NTSTATUS status = ObReferenceObjectByName(&name, 0, IoDeviceObjectType, NULL, device);
    xsdk_log_write("Open optical device: 0x%08X", (unsigned)status);
    return status;
}

bool read_disc(DiscToc *disc, char *diagnostic, size_t capacity)
{
    uint32_t device;
    const NTSTATUS opened = open_optical(&device);
    if (!NT_SUCCESS(opened) || !device) {
        snprintf(diagnostic, capacity, "Drive open: 0x%08X\nSee default.log for details.", (unsigned)opened);
        return false;
    }

    _Alignas(128) uint8_t bytes[CD_TOC_BYTES] = { 0 };
    uint32_t size = 0;
    const NTSTATUS legacy = IoSynchronousDeviceIoControlRequest(
        READ_TOC, device, NULL, 0, bytes, sizeof(bytes), &size);
    if (decode_toc(READ_TOC, legacy, size, bytes, disc)) {
        ObDereferenceObject(device);
        return true;
    }

    const NTSTATUS media = IoSynchronousDeviceIoControlRequest(CHECK_MEDIA, device, NULL, 0, NULL, 0, NULL);
    xsdk_log_write("Check media: 0x%08X", (unsigned)media);

    // Request the formatted TOC in minute/second/frame units.
    const uint8_t format[4] = { 0x80, 0, 0, 0 };
    memset(bytes, 0, sizeof(bytes));
    size = 0;
    const NTSTATUS extended = IoSynchronousDeviceIoControlRequest(
        READ_TOC_EX, device, format, sizeof(format), bytes, sizeof(bytes), &size);
    if (decode_toc(READ_TOC_EX, extended, size, bytes, disc)) {
        ObDereferenceObject(device);
        return true;
    }

    // Some 360 drivers report DATA_OVERRUN when the formatted TOC is shorter than the allocation.
    uint8_t scsi_status;
    NTSTATUS passthrough = scsi_read_toc(device, bytes, sizeof(bytes), &scsi_status, &size);
    const bool initial = decode_toc(SCSI_PASS_THROUGH_DIRECT, passthrough, size, bytes, disc) && scsi_status == 0;
    const uint32_t reported = ((uint32_t)bytes[0] << 8 | bytes[1]) + 2;
    bool exact = false;
    if (passthrough == DATA_OVERRUN && scsi_status == 0 && reported >= 20 && reported < sizeof(bytes) &&
        (reported - 4) % 8 == 0) {
        DiscToc retry;
        passthrough = scsi_read_toc(device, bytes, reported, &scsi_status, &size);
        exact = decode_toc(SCSI_PASS_THROUGH_DIRECT, passthrough, size, bytes, &retry) && scsi_status == 0;
        if (exact)
            *disc = retry;
    }
    ObDereferenceObject(device);
    if (exact || initial)
        return true;

    if (size >= 4) {
        const unsigned limit = size < CD_TOC_BYTES ? size : CD_TOC_BYTES;
        for (unsigned entry = 4; entry + 8 <= limit && entry < reported; entry += 8) {
            xsdk_log_write("TOC descriptor: ctrl=%02X track=%02X address=%02X %02X %02X %02X",
                bytes[entry + 1], bytes[entry + 2], bytes[entry + 4], bytes[entry + 5], bytes[entry + 6], bytes[entry + 7]);
        }
    }

    snprintf(diagnostic, capacity,
        "Media:     0x%08X\nLegacy:    0x%08X\nExtended:  0x%08X\nSCSI:      0x%08X / %02X\n\n"
        "See default.log for the returned bytes.",
        (unsigned)media, (unsigned)legacy, (unsigned)extended, (unsigned)passthrough, (unsigned)scsi_status);
    return false;
}

bool read_audio_sectors(uint32_t device, uint32_t lba, uint8_t sectors, uint8_t *data)
{
    const uint32_t bytes = sectors * CD_SECTOR_BYTES;
    // MMC READ CD, expected sector type CD-DA, user data only.
    ScsiRead request = {
        .length = sizeof(ScsiRead), .cdb_length = 12, .direction = SCSI_DATA_IN,
        .transfer_bytes = bytes, .timeout_seconds = 4, .data = data,
        .cdb = { SCSI_READ_CD, 0x04, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
            (uint8_t)(lba >> 8), (uint8_t)lba, 0, 0, sectors, 0x10, 0, 0 }
    };
    const NTSTATUS status = IoSynchronousDeviceIoControlRequest(
        SCSI_PASS_THROUGH_DIRECT, device, &request, sizeof(request), &request, sizeof(request), NULL);
    if (!NT_SUCCESS(status) || request.scsi_status || request.transfer_bytes != bytes) {
        xsdk_log_write("READ CD: lba=%u sectors=%u status=0x%08X scsi=0x%02X bytes=%u",
            (unsigned)lba, (unsigned)sectors, (unsigned)status,
            (unsigned)request.scsi_status, (unsigned)request.transfer_bytes);
        return false;
    }
    return true;
}

uint32_t drive_tray_state(void)
{
    typedef uint32_t (*TrayState)(void);
    static TrayState state;
    if (!state) {
        HANDLE module;
        void *address = NULL;
        if (XexGetModuleHandle("xam.xex", &module) != ERROR_SUCCESS ||
            !NT_SUCCESS(XexGetProcedureAddress(module, 426, &address)))
            return TRAY_UNKNOWN;
        state = (TrayState)address;
    }
    return state ? state() : TRAY_UNKNOWN;
}

bool drive_set_tray(bool open)
{
    typedef void (*SetTray)(uint32_t);
    HANDLE module;
    void *address = NULL;
    if (XexGetModuleHandle("xboxkrnl.exe", &module) != ERROR_SUCCESS ||
        !NT_SUCCESS(XexGetProcedureAddress(module, 36, &address)) || !address)
        return false;
    ((SetTray)address)(open ? 1u : 0u);
    return true;
}
