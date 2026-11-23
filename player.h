#ifndef DISCCUE_PLAYER_H
#define DISCCUE_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PLAYBACK_IDLE,
    PLAYBACK_BUFFERING,
    PLAYBACK_PLAYING,
    PLAYBACK_PAUSED,
    PLAYBACK_FINISHED,
    PLAYBACK_FAILED,
} PlaybackState;

bool playback_start(uint32_t first_frame, uint32_t end_frame);
void playback_stop(void);
void playback_pause(bool pause);
PlaybackState playback_state(void);
unsigned playback_seconds(void);

#endif
