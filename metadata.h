#ifndef DISCCUE_METADATA_H
#define DISCCUE_METADATA_H

#include "ui.h"

enum { NAME_BYTES = 261 };
typedef struct {
    char album[NAME_BYTES], artist[NAME_BYTES], tracks[CD_TRACK_LIMIT][NAME_BYTES];
    const char *notice;
    uint8_t cover[COVER_BYTES];
    bool has_cover;
} DiscMetadata;

void load_metadata(const DiscToc *disc, DiscMetadata *metadata);

#endif
