#ifndef DISCCUE_TOC_H
#define DISCCUE_TOC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { CD_TRACK_LIMIT = 99, CD_TOC_BYTES = 804 };

typedef struct {
    unsigned first;
    unsigned last;
    unsigned leadout;
    unsigned offset[CD_TRACK_LIMIT];
} DiscToc;

bool parse_toc(const uint8_t *bytes, size_t length, DiscToc *disc);
size_t format_toc(const DiscToc *disc, char *output, size_t capacity);

#endif
