#include "toc.h"

#include <stdio.h>

bool parse_toc(const uint8_t *bytes, size_t length, DiscToc *disc)
{
    if (length < 4)
        return false;

    const size_t reported = ((size_t)bytes[0] << 8 | bytes[1]) + 2;
    const unsigned first = bytes[2];
    const unsigned last = bytes[3];
    if (!first || last < first || last > CD_TRACK_LIMIT || reported > length ||
        reported < 4 + 8 * (last - first + 2))
        return false;

    DiscToc result = { .first = first, .last = last };
    unsigned previous = 0;
    for (unsigned track = first; track <= last + 1; ++track) {
        const uint8_t *entry = bytes + 4 + 8 * (track - first);
        const bool leadout = track == last + 1;
        if (entry[2] != (leadout ? 0xaa : track) || (!leadout && (entry[1] & 4)) ||
            entry[5] > 99 || entry[6] >= 60 || entry[7] >= 75)
            return false;
        const unsigned frame = (entry[5] * 60u + entry[6]) * 75u + entry[7];
        if (frame < 150 || (track != first && frame <= previous))
            return false;
        if (leadout)
            result.leadout = frame;
        else
            result.offset[track - first] = frame;
        previous = frame;
    }

    *disc = result;
    return true;
}

size_t format_toc(const DiscToc *disc, char *output, size_t capacity)
{
    if (!disc->first || disc->last < disc->first || disc->last > CD_TRACK_LIMIT)
        return 0;
    int written = snprintf(output, capacity, "%u %u %u", disc->first, disc->last, disc->leadout);
    if (written < 0 || (size_t)written >= capacity)
        return 0;
    size_t used = (size_t)written;
    for (unsigned index = 0; index <= disc->last - disc->first; ++index) {
        written = snprintf(output + used, capacity - used, " %u", disc->offset[index]);
        if (written < 0 || (size_t)written >= capacity - used)
            return 0;
        used += (size_t)written;
    }
    return used;
}
