#include "player.h"

#include "drive.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <xsdk/audio.h>
#include <xsdk/log.h>
#include <xsdk/thread.h>

enum {
    RING_SECTORS = 96,
    PREFILL_SECTORS = 24,
    PCM_FRAME_BYTES = 4,
};

static struct {
    pthread_t thread;
    bool thread_live;
    uint32_t first_lba, sector_count;
    atomic_bool stop, paused, drive_failed;
    atomic_uint written, consumed, position;
    atomic_int state;
    uint8_t ring[RING_SECTORS][CD_SECTOR_BYTES];
} player;

static void *read_sectors(void *unused)
{
    (void)unused;
    uint32_t device;
    if (!NT_SUCCESS(open_optical(&device)) || !device) {
        atomic_store(&player.drive_failed, true);
        return NULL;
    }
    _Alignas(128) static uint8_t transfer[CD_SECTORS_PER_READ * CD_SECTOR_BYTES];
    unsigned written = 0;
    unsigned batch_limit = CD_SECTORS_PER_READ;
    while (written < player.sector_count && !atomic_load(&player.stop)) {
        const unsigned room = RING_SECTORS - (written - atomic_load(&player.consumed));
        if (!room) {
            xsdk_thread_sleep(2);
            continue;
        }
        unsigned batch = player.sector_count - written;
        if (batch > batch_limit)
            batch = batch_limit;
        if (batch > room)
            batch = room;
        if (!read_audio_sectors(device, player.first_lba + written, (uint8_t)batch, transfer)) {
            if (batch == 1) {
                atomic_store(&player.drive_failed, true);
                break;
            }
            batch_limit = 1;
            continue;
        }
        for (unsigned i = 0; i < batch; ++i)
            memcpy(player.ring[(written + i) % RING_SECTORS], transfer + i * CD_SECTOR_BYTES, CD_SECTOR_BYTES);
        written += batch;
        atomic_store(&player.written, written);
    }
    ObDereferenceObject(device);
    return NULL;
}

static int sample_at(unsigned sample, unsigned channel)
{
    const uint8_t *const bytes = player.ring[(sample / CD_SAMPLES_PER_SECTOR) % RING_SECTORS] +
        (sample % CD_SAMPLES_PER_SECTOR) * PCM_FRAME_BYTES + channel * 2;
    const int value = bytes[0] | (bytes[1] << 8);
    return value < 32768 ? value : value - 65536;
}

static void *play_track(void *unused)
{
    (void)unused;
    pthread_t reader;
    if (pthread_create(&reader, NULL, read_sectors, NULL)) {
        atomic_store(&player.state, PLAYBACK_FAILED);
        return NULL;
    }

    const unsigned prefill = player.sector_count < PREFILL_SECTORS ? player.sector_count : PREFILL_SECTORS;
    while (atomic_load(&player.written) < prefill && !atomic_load(&player.drive_failed) && !atomic_load(&player.stop))
        xsdk_thread_sleep(3);

    bool failed = atomic_load(&player.drive_failed);
    bool output_open = false;
    if (!failed && !atomic_load(&player.stop)) {
        const NTSTATUS status = xsdk_audio_open(12);
        xsdk_log_write("Audio output open: 0x%08X", (unsigned)status);
        output_open = NT_SUCCESS(status);
        failed = !output_open;
    }
    if (output_open) {
        atomic_store(&player.state, PLAYBACK_PLAYING);
        const unsigned input_count = player.sector_count * CD_SAMPLES_PER_SECTOR;
        unsigned input = 0, phase = 0, queued = 0;
        while (input < input_count && !atomic_load(&player.stop)) {
            if (atomic_load(&player.drive_failed)) {
                failed = true;
                break;
            }
            if (atomic_load(&player.paused)) {
                atomic_store(&player.state, PLAYBACK_PAUSED);
                xsdk_thread_sleep(5);
                continue;
            }
            atomic_store(&player.state, PLAYBACK_PLAYING);

            // A full output frame consumes at most 236 PCM samples, plus one for interpolation.
            const unsigned last_sample = input_count - input > 237 ? input + 236 : input_count - 1;
            if (atomic_load(&player.written) <= last_sample / CD_SAMPLES_PER_SECTOR) {
                xsdk_thread_sleep(1);
                continue;
            }

            float *frame;
            const NTSTATUS status = xsdk_audio_next_frame(250, &frame);
            if (!NT_SUCCESS(status)) {
                xsdk_log_write("Audio frame failed: 0x%08X", (unsigned)status);
                failed = true;
                break;
            }
            memset(frame, 0, XSDK_AUDIO_FRAME_FLOATS * sizeof(*frame));
            for (unsigned i = 0; i < XSDK_AUDIO_FRAME_SAMPLES && input < input_count; ++i, ++queued) {
                const unsigned next = input + (input + 1 < input_count);
                const float fraction = phase / 160.0f;
                for (unsigned channel = 0; channel < 2; ++channel) {
                    const int sample = sample_at(input, channel);
                    const int following = sample_at(next, channel);
                    frame[channel * XSDK_AUDIO_FRAME_SAMPLES + i] =
                        (sample + (following - sample) * fraction) * (0.60f / 32768.0f);
                }
                // Exact 44.1:48 kHz rate ratio, with interpolation across CD-sector boundaries.
                phase += 147;
                if (phase >= 160) {
                    phase -= 160;
                    ++input;
                }
            }
            xsdk_audio_queue_frame();
            atomic_store(&player.position, queued / XSDK_AUDIO_RATE_HZ);
            atomic_store(&player.consumed, input / CD_SAMPLES_PER_SECTOR);
        }
        if (!failed && !atomic_load(&player.stop))
            xsdk_thread_sleep(82);
        XsdkAudioStats stats;
        xsdk_audio_stats(&stats);
        xsdk_log_write("CD playback: LBA=%u sectors=%u, samples=%u, frames=%u underruns=%u",
            player.first_lba, player.sector_count, queued, (unsigned)stats.played, (unsigned)stats.silent);
        xsdk_audio_close();
    }
    const bool cancelled = atomic_load(&player.stop);
    atomic_store(&player.stop, true);
    pthread_join(reader, NULL);
    atomic_store(&player.state, cancelled ? PLAYBACK_IDLE : failed ? PLAYBACK_FAILED : PLAYBACK_FINISHED);
    return NULL;
}

bool playback_start(uint32_t first_frame, uint32_t end_frame)
{
    playback_stop();
    if (first_frame < 150 || end_frame <= first_frame)
        return false;
    player.first_lba = first_frame - 150;
    player.sector_count = end_frame - first_frame;
    atomic_store(&player.stop, false);
    atomic_store(&player.paused, false);
    atomic_store(&player.drive_failed, false);
    atomic_store(&player.written, 0);
    atomic_store(&player.consumed, 0);
    atomic_store(&player.position, 0);
    atomic_store(&player.state, PLAYBACK_BUFFERING);
    player.thread_live = pthread_create(&player.thread, NULL, play_track, NULL) == 0;
    if (!player.thread_live)
        atomic_store(&player.state, PLAYBACK_FAILED);
    return player.thread_live;
}

void playback_stop(void)
{
    if (player.thread_live) {
        atomic_store(&player.stop, true);
        pthread_join(player.thread, NULL);
        player.thread_live = false;
    }
    atomic_store(&player.state, PLAYBACK_IDLE);
    atomic_store(&player.position, 0);
}

void playback_pause(bool pause) { atomic_store(&player.paused, pause); }
PlaybackState playback_state(void) { return (PlaybackState)atomic_load(&player.state); }
unsigned playback_seconds(void) { return atomic_load(&player.position); }
