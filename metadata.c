#include "metadata.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <xsdk/log.h>

enum { LAN_PORT = 13660, HTTP_MAX = COVER_BYTES + 2048 };
static char reply[HTTP_MAX];

static bool helper_address(char *address, size_t size)
{
    FILE *config = fopen("game:\\helper-ip.txt", "r");
    if (!config)
        return false;
    const bool read = fgets(address, (int)size, config) != NULL;
    fclose(config);
    if (!read)
        return false;
    address[strcspn(address, " \t\r\n")] = 0;
    return *address != 0;
}

static int connect_to_helper(const struct sockaddr_in *address)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        goto failed;

    if (connect(fd, (const struct sockaddr *)address, sizeof(*address)) < 0) {
        if (errno != EINPROGRESS && errno != EAGAIN)
            goto failed;
        fd_set writable;
        fd_set errors;
        if (fd >= FD_SETSIZE)
            goto failed;
        FD_ZERO(&writable);
        FD_ZERO(&errors);
        FD_SET(fd, &writable);
        FD_SET(fd, &errors);
        struct timeval timeout = { .tv_sec = 4 };
        if (select(fd + 1, NULL, &writable, &errors, &timeout) <= 0 ||
            !FD_ISSET(fd, &writable) || FD_ISSET(fd, &errors))
            goto failed;
    }

    const struct timeval timeout = { .tv_sec = 15 };
    if (fcntl(fd, F_SETFL, flags) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0)
        goto failed;
    return fd;

failed:
    close(fd);
    return -1;
}

static char *http_get(const char *host, const char *path, char *response, size_t capacity, size_t *body_size)
{
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(LAN_PORT) };
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1)
        return NULL;
    const uint32_t ip = ntohl(address.sin_addr.s_addr);
    if ((ip >> 24) != 10 && (ip >> 20) != 0xac1 && (ip >> 16) != 0xc0a8 &&
        (ip >> 16) != 0xa9fe)
        return NULL;

    char request[900];
    const int length = snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    if (length < 0 || (size_t)length >= sizeof(request))
        return NULL;

    const int fd = connect_to_helper(&address);
    if (fd < 0) {
        xsdk_log_write("Connect failed: errno=%d", errno);
        return NULL;
    }
    int sent = 0;
    while (sent < length) {
        const ssize_t count = send(fd, request + sent, (size_t)(length - sent), 0);
        if (count <= 0) {
            xsdk_log_write("HTTP send failed: errno=%d", errno);
            close(fd);
            return NULL;
        }
        sent += (int)count;
    }

    size_t used = 0;
    ssize_t count = 0;
    while (used < capacity - 1) {
        count = recv(fd, response + used, capacity - 1 - used, 0);
        if (count <= 0)
            break;
        used += (size_t)count;
    }
    if (count != 0) {
        xsdk_log_write("HTTP receive incomplete: bytes=%u errno=%d", (unsigned)used, errno);
        close(fd);
        return NULL;
    }
    close(fd);
    response[used] = 0;
    if (strncmp(response, "HTTP/1.0 200 ", 13) && strncmp(response, "HTTP/1.1 200 ", 13))
        return NULL;
    char *body = strstr(response, "\r\n\r\n");
    if (!body)
        return NULL;
    body += 4;
    *body_size = used - (size_t)(body - response);
    return body;
}

static char *next_line(char **cursor)
{
    char *start = *cursor;
    char *end = strchr(start, '\n');
    if (!end)
        return NULL;
    *end = 0;
    *cursor = end + 1;
    return start;
}

static bool copy_name(char *output, char **body)
{
    const char *value = next_line(body);
    if (!value || strlen(value) >= NAME_BYTES)
        return false;
    strcpy(output, value);
    return true;
}

static bool release_id(const char *value)
{
    if (!value || strlen(value) != 36)
        return false;
    for (unsigned i = 0; i < 36; ++i) {
        const char c = value[i];
        const bool hyphen = i == 8 || i == 13 || i == 18 || i == 23;
        if (hyphen ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

void load_metadata(const DiscToc *disc, DiscMetadata *metadata)
{
    char host[40], toc[750], query[820];
    strcpy(metadata->album, "Audio CD");
    metadata->notice = "Set helper-ip.txt to enable album lookup.";
    if (!helper_address(host, sizeof(host)) || !format_toc(disc, toc, sizeof(toc)))
        return;
    for (char *p = toc; *p; ++p) {
        if (*p == ' ')
            *p = '+';
    }
    snprintf(query, sizeof(query), "/lookup?toc=%s", toc);
    size_t size;
    char *body = http_get(host, query, reply, sizeof(reply), &size);
    if (!body) {
        metadata->notice = "Mac helper unavailable. Audio playback is ready.";
        return;
    }
    const char *status = next_line(&body);
    if (!status || strcmp(status, "OK")) {
        metadata->notice = status && !strcmp(status, "NO_MATCH") ? "No MusicBrainz match for this pressing." :
            status && !strcmp(status, "AMBIGUOUS") ? "MusicBrainz found multiple track lists." :
            "Metadata unavailable. Audio playback is ready.";
        return;
    }
    if (!copy_name(metadata->album, &body) || !copy_name(metadata->artist, &body)) {
        strcpy(metadata->album, "Audio CD");
        metadata->notice = "The helper returned an incomplete track list.";
        return;
    }
    const unsigned total = disc->last - disc->first + 1;
    for (unsigned index = 0; index < total; ++index) {
        if (!copy_name(metadata->tracks[index], &body)) {
            metadata->notice = "The helper returned an incomplete track list.";
            return;
        }
    }
    xsdk_log_write("Album: %s | Artist: %s", metadata->album, metadata->artist);
    for (unsigned index = 0; index < total; ++index)
        xsdk_log_write("%02u %s", index + disc->first, metadata->tracks[index]);

    metadata->notice = "";
    const char *id = next_line(&body);
    if (!release_id(id))
        return;
    snprintf(query, sizeof(query), "/cover/%s", id);
    body = http_get(host, query, reply, sizeof(reply), &size);
    if (body && size == COVER_BYTES) {
        memcpy(metadata->cover, body, COVER_BYTES);
        metadata->has_cover = true;
        xsdk_log_write("Album artwork: %u bytes", (unsigned)size);
    }
}
