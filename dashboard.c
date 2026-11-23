#include <xsdk/plugin.h>
#include <stddef.h>

// Stock dashboard disc-launch call in the dashboard image checked on this console.
// The context words keep this plugin from editing a different dashboard build.
#define DISC_CALL_ADDRESS UINT32_C(0x92189a74)
#define STOCK_DISC_CALL UINT32_C(0x487b0fd1)
#define LAUNCH_THUNK_ADDRESS UINT32_C(0x9293aa44)

enum {
    XAM_MEDIA_INFO = 419,
    XAM_LAUNCH_TITLE_EX = 421,
    MEDIA_AUDIO_CD = 7,
    LAUNCH_DISC_FLAG = 2,
    EXECUTE_READWRITE = 0x40,
};

typedef void (*GetMediaInfo)(uint32_t *, uint32_t *);
typedef NTSTATUS (*LaunchTitle)(const char *, const char *, const char *, uint32_t);
static GetMediaInfo media_info;
static LaunchTitle launch_title;
static const char app_folder[] = "\\Device\\Harddisk0\\Partition1\\Apps\\DiscCue360";
static const char app_path[] = "\\Device\\Harddisk0\\Partition1\\Apps\\DiscCue360\\default.xex";

static bool player_present(void)
{
    const ANSI_STRING name = { sizeof(app_path) - 1, sizeof(app_path), app_path };
    const OBJECT_ATTRIBUTES attributes = { 0, &name, 0 };
    FILE_NETWORK_OPEN_INFORMATION information;
    return NT_SUCCESS(NtQueryFullAttributesFile(&attributes, &information));
}

static NTSTATUS play_disc(const char *path, const char *mount, const char *command, uint32_t flags)
{
    if (!path && !mount && !command && (flags & LAUNCH_DISC_FLAG)) {
        uint32_t type = 0, title = 0;
        media_info(&type, &title);
        xsdk_plugin_log("Play disc: media=%u title=%08X", (unsigned)type, (unsigned)title);
        if (type == MEDIA_AUDIO_CD && player_present()) {
            xsdk_plugin_log("Launching DiscCue 360");
            const NTSTATUS result = launch_title(app_path, app_folder, NULL, 0);
            if (NT_SUCCESS(result))
                return result;
            xsdk_plugin_log("DiscCue launch failed: %08X", (unsigned)result);
        }
    }
    return launch_title(path, mount, command, flags);
}

static bool is_known_dashboard(void)
{
    static const uint32_t expected[] = {
        0x63e60002, 0x38a00000, 0x38800000, 0x38600000,
        STOCK_DISC_CALL, 0x4bffea51, 0x3ba00000, 0x7fa3eb78,
    };
    const volatile uint32_t *const words = (const volatile uint32_t *)(uintptr_t)(DISC_CALL_ADDRESS - 16);
    for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        if (words[i] != expected[i])
            return false;
    }
    return true;
}

static bool writable_code(uint32_t protection)
{
    return protection == PAGE_READWRITE || protection == EXECUTE_READWRITE;
}

static bool install_branch(volatile uint32_t *site, uint32_t branch)
{
    *site = branch;
    __asm__ volatile("dcbst 0,%0; sync; icbi 0,%0; sync; isync" :: "r"(site) : "memory");
    return *site == branch;
}

static uint32_t make_branch(uint32_t address, bool link)
{
    const int32_t displacement = (int32_t)((uint32_t)(uintptr_t)play_disc - address);
    if (displacement < -0x02000000 || displacement > 0x01fffffc || displacement % 4)
        return 0;
    return 0x48000000 | ((uint32_t)displacement & 0x03fffffc) | (uint32_t)link;
}

static bool is_launch_thunk(const volatile uint32_t *thunk)
{
    const uint32_t upper = thunk[0];
    const uint32_t lower = thunk[1];
    if ((upper & 0xffff0000) != 0x3d600000 || thunk[2] != 0x7d6903a6 || thunk[3] != 0x4e800420)
        return false;

    uint32_t target = upper << 16;
    switch (lower & 0xffff0000) {
    case 0x616b0000:
        target |= lower & 0xffff;
        break;
    case 0x396b0000:
        target += (uint32_t)(int16_t)lower;
        break;
    default:
        return false;
    }
    return target == (uint32_t)(uintptr_t)launch_title;
}

void plugin_main(HANDLE module)
{
    (void)module;
    xsdk_plugin_log_open("\\Device\\Harddisk0\\Partition1\\Apps\\DiscCue360\\PlayCD.log");
    xsdk_plugin_log("DiscCue stock dashboard plugin v5");
    const int64_t delay = -5000000;
    HANDLE xam;
    for (unsigned attempt = 0; attempt < 12 && XexGetModuleHandle("xam.xex", &xam) != ERROR_SUCCESS; ++attempt)
        KeDelayExecutionThread(0, 0, &delay);
    void *launch = NULL, *query = NULL;
    if (XexGetModuleHandle("xam.xex", &xam) != ERROR_SUCCESS ||
        !NT_SUCCESS(XexGetProcedureAddress(xam, XAM_MEDIA_INFO, &query)) ||
        !NT_SUCCESS(XexGetProcedureAddress(xam, XAM_LAUNCH_TITLE_EX, &launch)) || !query || !launch) {
        xsdk_plugin_log("Dashboard launch services unavailable");
        return;
    }
    media_info = (GetMediaInfo)query;
    launch_title = (LaunchTitle)launch;
    const uint32_t call_branch = make_branch(DISC_CALL_ADDRESS, true);
    const uint32_t thunk_branch = make_branch(LAUNCH_THUNK_ADDRESS, false);
    volatile uint32_t *const call = (volatile uint32_t *)(uintptr_t)DISC_CALL_ADDRESS;
    volatile uint32_t *const thunk = (volatile uint32_t *)(uintptr_t)LAUNCH_THUNK_ADDRESS;
    bool refused = false;
    for (;;) {
        const bool mapped = MmIsAddressValid((const void *)call);
        const bool thunk_mapped = mapped && MmIsAddressValid((const void *)thunk);
        if (!mapped) {
            refused = false;
        } else if ((!call_branch || *call != call_branch) &&
            (!thunk_branch || !thunk_mapped || *thunk != thunk_branch) && is_known_dashboard()) {
            const uint32_t call_protection = MmQueryAddressProtect(DISC_CALL_ADDRESS);
            const uint32_t thunk_protection = thunk_mapped ? MmQueryAddressProtect(LAUNCH_THUNK_ADDRESS) : 0;
            const bool thunk_matches = thunk_mapped && is_launch_thunk(thunk);
            if (call_branch && thunk_matches && writable_code(call_protection) && install_branch(call, call_branch)) {
                xsdk_plugin_log("Installed stock Play Disc call redirect");
            } else if (thunk_branch && thunk_matches && writable_code(thunk_protection) &&
                install_branch(thunk, thunk_branch)) {
                xsdk_plugin_log("Installed stock dashboard disc-launch import redirect");
            } else if (!refused) {
                uint32_t type = 0, title = 0;
                media_info(&type, &title);
                xsdk_plugin_log("No writable redirect: call=%08X import=%08X import matches=%u disc type=%u",
                    (unsigned)call_protection, (unsigned)thunk_protection, (unsigned)thunk_matches, (unsigned)type);
                refused = true;
            }
        }
        KeDelayExecutionThread(0, 0, &delay);
    }
}
