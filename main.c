#include "drive.h"
#include "metadata.h"
#include "player.h"
#include "ui.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <xsdk/gpu.h>
#include <xsdk/log.h>
#include <xsdk/present.h>
#include <xsdk/texture.h>
#include <xsdk/thread.h>
#include <xsdk/xam.h>

static uint32_t pixels[UI_WIDTH * UI_HEIGHT];
static XsdkTexture front;

static struct {
    pthread_t thread;
    atomic_bool disc_checked, complete;
    DiscToc disc;
    DiscMetadata metadata;
    bool busy, readable, published;
    char diagnostic[280];
} scan;

static bool active_disc;
static unsigned selection;
static unsigned playing = CD_TRACK_LIMIT;
static bool loop;
static bool scan_pending, discard_scan;
static bool tray_open;
static const char *notice = "Insert an audio CD.";
static uint32_t tray_state = TRAY_UNKNOWN;
static uint32_t last_tray_poll, next_scan;
static unsigned scan_retries;
static uint16_t previous_buttons;
static uint32_t repeat_at;
static int8_t stick_direction[4];

static uint32_t milliseconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)now.tv_sec * 1000u + (uint32_t)now.tv_nsec / 1000000u;
}

static bool open_screen(void)
{
    uint32_t width = 0, height = 0;
    NTSTATUS status = xsdk_gpu_open();
    if (NT_SUCCESS(status))
        status = xsdk_present_open(XSDK_DISPLAY_720P, 2, 1, &width, &height);
    if (NT_SUCCESS(status) && width == UI_WIDTH && height == UI_HEIGHT)
        status = xsdk_texture_init(&front, XSDK_FORMAT_A8R8G8B8, width, height, 1, true);
    else if (NT_SUCCESS(status))
        status = STATUS_INVALID_DEVICE_REQUEST;
    if (!NT_SUCCESS(status))
        xsdk_log_write("Display init failed: 0x%08X", (unsigned)status);
    return NT_SUCCESS(status);
}

static bool render(void)
{
    const bool metadata_ready = active_disc && !scan.busy;
    PlayerView view = {
        .toc = active_disc ? &scan.disc : NULL,
        .album = metadata_ready ? scan.metadata.album : "Audio CD",
        .artist = metadata_ready ? scan.metadata.artist : "",
        .notice = notice,
        .cover = metadata_ready && scan.metadata.has_cover ? scan.metadata.cover : NULL,
        .selected = selection, .playing = playing, .seconds = playback_seconds(),
        .state = playback_state(), .loop = loop, .scanning = scan.busy, .tray_open = tray_open,
    };
    if (metadata_ready) {
        for (unsigned i = 0; i <= scan.disc.last - scan.disc.first; ++i)
            view.titles[i] = scan.metadata.tracks[i];
    }
    ui_draw(pixels, &view);
    uint32_t address;
    const NTSTATUS status = xsdk_present_begin(&address, 1500);
    if (!NT_SUCCESS(status)) {
        xsdk_log_write("Frame failed: 0x%08X", (unsigned)status);
        return false;
    }
    xsdk_texture_attach(&front, xsdk_present_pixels(address));
    xsdk_texture_upload(&front, 0, pixels, UI_WIDTH * sizeof(*pixels));
    __asm__ volatile("sync" ::: "memory");
    xsdk_present_end();
    return true;
}

static void *scan_disc(void *unused)
{
    (void)unused;
    scan.readable = read_disc(&scan.disc, scan.diagnostic, sizeof(scan.diagnostic));
    atomic_store(&scan.disc_checked, true);
    if (scan.readable) {
        char toc[750];
        if (format_toc(&scan.disc, toc, sizeof(toc)))
            xsdk_log_write("TOC %s", toc);
        load_metadata(&scan.disc, &scan.metadata);
    }
    atomic_store(&scan.complete, true);
    return NULL;
}

static void stop_track(void)
{
    playback_stop();
    playing = CD_TRACK_LIMIT;
}

static void start_scan(void)
{
    scan_pending = false;
    stop_track();
    active_disc = false;
    selection = 0;
    discard_scan = false;
    memset(&scan.metadata, 0, sizeof(scan.metadata));
    atomic_store(&scan.disc_checked, false);
    atomic_store(&scan.complete, false);
    scan.published = false;
    scan.busy = pthread_create(&scan.thread, NULL, scan_disc, NULL) == 0;
    notice = scan.busy ? "Checking the disc and looking up the album…" : "Could not start the disc read.";
}

static void schedule_scan(uint32_t now)
{
    scan_pending = true;
    next_scan = now;
    scan_retries = 2;
}

static void poll_scan(uint32_t now)
{
    if (scan.busy && !scan.published && !discard_scan && atomic_load(&scan.disc_checked)) {
        scan.published = true;
        active_disc = scan.readable;
        if (active_disc)
            notice = "Looking up track names and cover art…";
    }
    if (scan.busy && atomic_load(&scan.complete)) {
        pthread_join(scan.thread, NULL);
        scan.busy = false;
        if (!discard_scan) {
            active_disc = scan.readable;
            if (active_disc) {
                notice = scan.metadata.notice;
            } else if (tray_state == TRAY_READY && scan_retries) {
                --scan_retries;
                scan_pending = true;
                next_scan = now + 450;
                notice = "Waiting for the drive…";
            } else {
                notice = "No readable audio CD in the drive.";
            }
        }
    }
    if (scan_pending && !scan.busy && !tray_open && (int32_t)(now - next_scan) >= 0)
        start_scan();
}

static void poll_tray(uint32_t now)
{
    if (now - last_tray_poll < 250)
        return;
    last_tray_poll = now;
    const uint32_t state = drive_tray_state();
    if (state == tray_state || state == TRAY_UNKNOWN)
        return;
    tray_state = state;
    tray_open = state == TRAY_OPEN;
    if (tray_open) {
        stop_track();
        active_disc = false;
        discard_scan = true;
        scan_pending = false;
        notice = "Tray open. Insert an audio CD.";
    } else if (state == TRAY_READY) {
        schedule_scan(now);
    }
}

static void tray_action(void)
{
    if (scan.busy && !atomic_load(&scan.disc_checked)) {
        notice = "Wait for this disc read to finish, then use LB.";
        return;
    }
    stop_track();
    if (!drive_set_tray(!tray_open)) {
        notice = "Tray control unavailable. Use the console’s eject button.";
        return;
    }
    const bool opening = !tray_open;
    active_disc = false;
    scan_pending = false;
    discard_scan = true;
    notice = opening ? "Opening tray…" : "Closing tray…";
}

static void play_selected(void)
{
    const unsigned last = scan.disc.last - scan.disc.first;
    const uint32_t end = selection < last ? scan.disc.offset[selection + 1] : scan.disc.leadout;
    if (playback_start(scan.disc.offset[selection], end)) {
        playing = selection;
        if (!scan.busy)
            notice = scan.metadata.notice;
    } else {
        playing = CD_TRACK_LIMIT;
        notice = "Could not start playback. See default.log.";
    }
}

static int8_t vertical_direction(int16_t axis, int8_t previous)
{
    enum { ENGAGE = 14000, RELEASE = 8500 };
    if (axis >= ENGAGE || (previous > 0 && axis >= RELEASE))
        return 1;
    if (axis <= -ENGAGE || (previous < 0 && axis <= -RELEASE))
        return -1;
    return 0;
}

static uint16_t buttons(uint32_t now)
{
    uint16_t held = 0;
    const uint16_t directions = INPUT_GAMEPAD_DPAD_UP | INPUT_GAMEPAD_DPAD_DOWN;
    for (uint32_t port = 0; port < 4; ++port) {
        INPUT_STATE input;
        if (XamInputGetState(port, 0, &input) != ERROR_SUCCESS) {
            stick_direction[port] = 0;
            continue;
        }
        stick_direction[port] = vertical_direction(input.gamepad.thumb_ly, stick_direction[port]);
        held |= input.gamepad.buttons;
        if (!(input.gamepad.buttons & directions)) {
            if (stick_direction[port] > 0)
                held |= INPUT_GAMEPAD_DPAD_UP;
            else if (stick_direction[port] < 0)
                held |= INPUT_GAMEPAD_DPAD_DOWN;
        }
    }
    uint16_t pressed = held & ~previous_buttons;
    if (pressed & directions)
        repeat_at = now + 340;
    else if ((held & directions) && (int32_t)(now - repeat_at) >= 0) {
        pressed |= held & directions;
        repeat_at = now + 85;
    }
    previous_buttons = held;
    return xsdk_system_ui_open() ? 0 : pressed;
}

static void controls(uint16_t pressed)
{
    if (pressed & INPUT_GAMEPAD_LEFT_SHOULDER)
        tray_action();
    if (pressed & INPUT_GAMEPAD_X)
        stop_track();
    if (pressed & INPUT_GAMEPAD_Y)
        loop = !loop;
    if (!active_disc || scan_pending)
        return;

    const unsigned last = scan.disc.last - scan.disc.first;
    if ((pressed & INPUT_GAMEPAD_DPAD_UP) && selection)
        --selection;
    if ((pressed & INPUT_GAMEPAD_DPAD_DOWN) && selection < last)
        ++selection;
    if (pressed & INPUT_GAMEPAD_A) {
        const PlaybackState state = playback_state();
        if (playing == selection && (state == PLAYBACK_PLAYING || state == PLAYBACK_BUFFERING))
            playback_pause(true);
        else if (playing == selection && state == PLAYBACK_PAUSED)
            playback_pause(false);
        else
            play_selected();
    }
    const PlaybackState state = playback_state();
    if (playing != CD_TRACK_LIMIT && state == PLAYBACK_FINISHED) {
        if (loop || playing < last) {
            selection = loop ? playing : playing + 1;
            play_selected();
        } else {
            stop_track();
        }
    } else if (playing != CD_TRACK_LIMIT && state == PLAYBACK_FAILED) {
        stop_track();
        notice = "Playback stopped after a drive or audio error. See default.log.";
    }
}

int main(void)
{
    xsdk_log_write("DiscCue 360 v0.6.1 CD player");
    if (!open_screen())
        return 1;
    tray_state = drive_tray_state();
    tray_open = tray_state == TRAY_OPEN;
    if (!tray_open)
        schedule_scan(milliseconds());
    while (true) {
        const uint32_t now = milliseconds();
        const uint16_t pressed = buttons(now);
        if (pressed & (INPUT_GAMEPAD_B | INPUT_GAMEPAD_BACK))
            break;
        poll_tray(now);
        poll_scan(now);
        controls(pressed);
        if (!render())
            break;
        const uint32_t spent = milliseconds() - now;
        if (spent < 30)
            xsdk_thread_sleep(30 - spent);
    }
    stop_track();
    if (scan.busy)
        pthread_join(scan.thread, NULL);
    return 0;
}
