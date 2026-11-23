#ifndef DISCCUE_UI_H
#define DISCCUE_UI_H

#include "player.h"
#include "toc.h"

enum { UI_WIDTH = 1280, UI_HEIGHT = 720, COVER_EDGE = 256, COVER_BYTES = COVER_EDGE * COVER_EDGE * 3 };

typedef struct {
    const DiscToc *toc;
    const char *album, *artist, *notice;
    const char *titles[CD_TRACK_LIMIT];
    const uint8_t *cover;
    unsigned selected, playing, seconds;
    PlaybackState state;
    bool loop, scanning, tray_open;
} PlayerView;

void ui_draw(uint32_t *pixels, const PlayerView *view);

#endif
