#include "ui.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    uint16_t code;
    uint32_t offset;
    int8_t x, y;
    uint8_t width, height, advance;
} Glyph;

#include "font_data.h"

typedef struct { const Glyph *glyphs; const uint8_t *alpha; } Font;
static const Font fonts[] = {
    {glyphs_0, alpha_0}, {glyphs_1, alpha_1}, {glyphs_2, alpha_2},
};

static const uint32_t background = 0x11181e, panel = 0x1a252d, raised = 0x23333c;
static const uint32_t line = 0x2b404b, bright = 0xf2f8fb, subdued = 0x9bb0bd, accent = 0x9bea73;
static const uint32_t highlight = 0x314b36;
static uint32_t *frame;

// The display expects four bytes per pixel in B,G,R,A order on the Xbox.
static uint32_t display_color(uint32_t rgb)
{
    return ((rgb & 0xff) << 24) | ((rgb & 0xff00) << 8) | ((rgb & 0xff0000) >> 8) | 0xffu;
}

static void rect(int x, int y, int width, int height, uint32_t rgb)
{
    const uint32_t color = display_color(rgb);
    for (int row = y; row < y + height; ++row) {
        uint32_t *pixel = frame + row * UI_WIDTH + x;
        for (int column = 0; column < width; ++column)
            pixel[column] = color;
    }
}

static void blend(int x, int y, uint32_t color, unsigned opacity)
{
    if (!opacity || x < 0 || x >= UI_WIDTH || y < 0 || y >= UI_HEIGHT)
        return;
    uint32_t *pixel = &frame[y * UI_WIDTH + x];
    if (opacity == 255) {
        *pixel = color;
        return;
    }
    const unsigned inverse = 255 - opacity;
    uint32_t result = 0xff;
    for (unsigned shift = 8; shift <= 24; shift += 8) {
        const unsigned value = (((color >> shift) & 255) * opacity + ((*pixel >> shift) & 255) * inverse) / 255;
        result |= value << shift;
    }
    *pixel = result;
}

static unsigned next_character(const unsigned char **text)
{
    const unsigned char *p = *text;
    unsigned code = *p++;
    if (code >= 0xc2 && code <= 0xf4) {
        unsigned left = code < 0xe0 ? 1 : code < 0xf0 ? 2 : 3;
        code &= 0x3fu >> left;
        while (left && (*p & 0xc0) == 0x80) {
            code = (code << 6) | (*p++ & 0x3f);
            --left;
        }
        if (left)
            code = '?';
    }
    *text = p;
    return code;
}

static const Glyph *glyph_at(const Font *font, unsigned code)
{
    const unsigned ascii = 127 - 32, latin = 256 - 160;
    if (code >= 32 && code < 127)
        return &font->glyphs[code - 32];
    if (code >= 160 && code < 256)
        return &font->glyphs[ascii + code - 160];
    for (unsigned i = ascii + latin; i < ascii + latin + 5; ++i) {
        if (font->glyphs[i].code == code)
            return &font->glyphs[i];
    }
    return &font->glyphs['?' - 32];
}

static void draw_glyph(const Font *font, const Glyph *glyph, int x, int baseline, uint32_t pixel)
{
    const uint8_t *alpha = font->alpha + glyph->offset;
    for (unsigned row = 0; row < glyph->height; ++row) {
        for (unsigned column = 0; column < glyph->width; ++column)
            blend(x + glyph->x + (int)column, baseline + glyph->y + (int)row, pixel, *alpha++);
    }
}

static void text(int x, int baseline, unsigned size, uint32_t rgb, const char *value, int limit)
{
    const Font *font = &fonts[size];
    const Glyph *dots = glyph_at(font, 0x2026);
    const uint32_t pixel = display_color(rgb);
    const unsigned char *cursor = (const unsigned char *)value;
    const int right = x + limit;
    while (*cursor) {
        const Glyph *glyph = glyph_at(font, next_character(&cursor));
        const int needed = glyph->x + glyph->width > glyph->advance ? glyph->x + glyph->width : glyph->advance;
        if (x + needed + (*cursor ? dots->advance : 0) > right) {
            if (x + dots->advance <= right)
                draw_glyph(font, dots, x, baseline, pixel);
            break;
        }
        draw_glyph(font, glyph, x, baseline, pixel);
        x += glyph->advance;
    }
}

static void disk_art(const uint8_t *cover)
{
    rect(81, 153, 270, 270, line);
    rect(88, 160, COVER_EDGE, COVER_EDGE, raised);
    if (cover) {
        for (unsigned y = 0; y < COVER_EDGE; ++y) {
            uint32_t *destination = frame + (160 + y) * UI_WIDTH + 88;
            for (unsigned x = 0; x < COVER_EDGE; ++x, cover += 3)
                destination[x] = ((uint32_t)cover[0] << 24) | ((uint32_t)cover[1] << 16) |
                    ((uint32_t)cover[2] << 8) | 255u;
        }
        return;
    }
    const uint32_t fill = display_color(0x39604a);
    const uint32_t inner = display_color(0x223b33);
    for (int y = -66; y <= 66; ++y) {
        for (int x = -66; x <= 66; ++x) {
            const int radius = x * x + y * y;
            if (radius <= 4356 && radius >= 256)
                frame[(282 + y) * UI_WIDTH + 216 + x] = radius < 2809 ? inner : fill;
        }
    }
    rect(114, 396, 204, 2, line);
}

static const char *playback_label(PlaybackState state)
{
    switch (state) {
    case PLAYBACK_BUFFERING: return "Buffering";
    case PLAYBACK_PLAYING: return "Playing";
    case PLAYBACK_PAUSED: return "Paused";
    case PLAYBACK_FAILED: return "Playback error";
    default: return "Stopped";
    }
}

static unsigned track_seconds(const DiscToc *disc, unsigned index)
{
    const unsigned end = index < disc->last - disc->first ? disc->offset[index + 1] : disc->leadout;
    return (end - disc->offset[index]) / 75;
}

static void chip(int x, int width, const char *button, const char *label, bool selected)
{
    rect(x, 618, width, 47, selected ? highlight : raised);
    rect(x + 10, 627, 36, 29, selected ? accent : panel);
    text(x + 19, 648, 0, selected ? background : bright, button, 27);
    text(x + 57, 648, 0, bright, label, width - 63);
}

void ui_draw(uint32_t *pixels, const PlayerView *view)
{
    frame = pixels;
    rect(0, 0, UI_WIDTH, UI_HEIGHT, background);
    rect(62, 51, 6, 39, accent);
    text(83, 83, 2, bright, "DiscCue 360", 435);
    text(1014, 79, 0, accent, view->tray_open ? "TRAY OPEN" : "AUDIO CD", 200);
    rect(62, 109, 1156, 1, line);
    rect(62, 132, 308, 455, panel);
    rect(394, 132, 824, 455, panel);
    disk_art(view->cover);

    text(84, 460, 1, bright, view->album, 266);
    text(84, 488, 0, subdued, view->artist, 266);
    text(84, 525, 0, accent, playback_label(view->state), 264);
    rect(84, 537, 264, 4, line);

    const DiscToc *disc = view->toc;
    const bool active = disc && view->playing != CD_TRACK_LIMIT;
    if (active) {
        const unsigned duration = track_seconds(disc, view->playing);
        const unsigned seconds = view->seconds > duration ? duration : view->seconds;
        if (duration)
            rect(84, 537, (int)(264 * seconds / duration), 4, accent);
        char time[64];
        snprintf(time, sizeof(time), "%u:%02u / %u:%02u", seconds / 60, seconds % 60, duration / 60, duration % 60);
        text(84, 568, 0, subdued, time, 255);
    }

    text(416, 165, 1, bright, "Track list", 365);
    if (disc) {
        const unsigned total = disc->last - disc->first + 1;
        text(552, 163, 0, subdued, "Left stick / D-pad", 250);
        char count[32];
        snprintf(count, sizeof(count), "%u tracks", total);
        text(1081, 163, 0, subdued, count, 110);
        const unsigned page = view->selected / 8 * 8;
        for (unsigned index = page; index < total && index < page + 8; ++index) {
            const int y = 181 + (int)(index - page) * 43;
            const bool selected = index == view->selected;
            if (selected) {
                rect(410, y, 792, 40, highlight);
                rect(410, y, 4, 40, accent);
            } else {
                rect(420, y + 40, 770, 1, line);
            }
            char number[6], duration[20];
            const unsigned seconds = track_seconds(disc, index);
            snprintf(number, sizeof(number), "%02u", index + disc->first);
            snprintf(duration, sizeof(duration), "%u:%02u", seconds / 60, seconds % 60);
            text(430, y + 26, 0, selected || index == view->playing ? accent : subdued, number, 40);
            const char *name = view->titles[index] && *view->titles[index] ? view->titles[index] : "Untitled track";
            text(482, y + 27, 1, bright, name, 569);
            text(1114, y + 26, 0, subdued, duration, 79);
            if (active && index == view->playing)
                rect(1072, y + 14, 5, 12, accent);
        }
    } else {
        text(418, 232, 1, subdued, view->scanning ? "Reading the disc…" : "Insert an audio CD", 740);
    }
    text(417, 564, 0, subdued, view->notice, 770);

    const bool can_pause = active && view->playing == view->selected &&
        (view->state == PLAYBACK_PLAYING || view->state == PLAYBACK_BUFFERING);
    chip(62, 220, "A", can_pause ? "Pause" : active && view->playing == view->selected &&
        view->state == PLAYBACK_PAUSED ? "Resume" : "Play", true);
    chip(296, 220, "X", "Stop", false);
    chip(530, 220, "Y", view->loop ? "Loop on" : "Loop off", view->loop);
    chip(764, 220, "LB", view->tray_open ? "Close tray" : "Eject", false);
    chip(998, 220, "B", "Exit", false);
}
