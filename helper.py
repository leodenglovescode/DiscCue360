#!/usr/bin/env python3
"""Resolve audio CD tables of contents via MusicBrainz for DiscCue 360."""

import argparse
import base64
import hashlib
import http.server
import io
import ipaddress
import json
import os
import re
import socket
import struct
import subprocess
import tempfile
import sys
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
from collections import OrderedDict
from pathlib import Path

USER_AGENT = "DiscCue360/0.1 (Xbox 360 audio CD metadata prototype)"
API = "https://musicbrainz.org/ws/2/discid/"
COVER = "https://coverartarchive.org/"
COVER_SIZE = 256
COVER_BYTES = COVER_SIZE * COVER_SIZE * 3
MAX_CACHE_TEXT = 32 * 1024
RELEASE_ID = re.compile(r"[a-f0-9]{8}(-[a-f0-9]{4}){3}-[a-f0-9]{12}\Z")


def disc_id(toc):
    first, last, leadout, *offsets = toc
    if not (1 <= first <= last <= 99 and len(offsets) == last - first + 1):
        raise ValueError("invalid track range")
    frames = [*offsets, leadout]
    if not (150 <= frames[0] < frames[-1] < 450000) or any(
        left >= right for left, right in zip(frames, frames[1:])
    ):
        raise ValueError("invalid frame offsets")
    positions = [0] * 99
    positions[first - 1 : last] = offsets
    canonical = f"{first:02X}{last:02X}{leadout:08X}" + "".join(f"{x:08X}" for x in positions)
    digest = hashlib.sha1(canonical.encode("ascii")).digest()
    return base64.b64encode(digest).decode("ascii").translate(str.maketrans("+/=", "._-"))


def clean(value):
    text = "".join(" " if unicodedata.category(c)[0] == "C" else c for c in value)
    return " ".join(text.split())[:65] or "Unknown"


def select_matches(payload, disc, track_count):
    matches = {}
    recordings = set()
    for release in payload.get("releases", []):
        for medium in release.get("media", []):
            tracks = medium.get("tracks", [])
            if len(tracks) != track_count:
                continue
            discs = medium.get("discs", [])
            if not any(item.get("id") == disc for item in discs):
                continue
            artist = "".join(
                part.get("name", part.get("artist", {}).get("name", "")) + part.get("joinphrase", "")
                for part in release.get("artist-credit", [])
            )
            album = clean(release.get("title", ""))
            titles = tuple(clean(track.get("title", track.get("recording", {}).get("title", ""))) for track in tracks)
            key = (album, clean(artist), titles)
            group_id = release.get("release-group", {}).get("id")
            track_ids = tuple(track.get("recording", {}).get("id") for track in tracks)
            recordings.add((key[:2], group_id, track_ids) if group_id and all(track_ids) else None)
            release_id = release.get("id", "")
            if release.get("cover-art-archive", {}).get("front") and RELEASE_ID.fullmatch(release_id):
                matches[key] = min(matches.get(key) or release_id, release_id)
            else:
                matches.setdefault(key, "")
    stub = payload.get("cdstub")
    if not matches and isinstance(stub, dict):
        tracks = stub.get("track-list", stub.get("tracks", []))
        if len(tracks) == track_count:
            matches[(
                clean(stub.get("title", "")),
                clean(stub.get("artist", "")),
                tuple(clean(track.get("title", "")) for track in tracks),
            )] = ""
    # Editions can spell the same recordings differently, even with an identical disc ID.
    if len(matches) > 1 and (len(recordings) != 1 or None in recordings):
        return ("AMBIGUOUS",)
    if not matches:
        return ("NO_MATCH",)
    album, artist, titles = min(matches, key=lambda key: (not matches[key], matches[key], key))
    return ("OK", album, artist, *titles, matches[(album, artist, titles)])


class DiskCache:
    def __init__(self, directory):
        self.directory = directory
        if directory:
            directory.mkdir(parents=True, exist_ok=True)

    def read(self, name, limit):
        if not self.directory:
            return None
        try:
            with (self.directory / name).open("rb") as file:
                data = file.read(limit + 1)
            return data if len(data) <= limit else None
        except FileNotFoundError:
            return None
        except OSError as error:
            print(f"Could not read cache entry {name}: {error}", file=sys.stderr)
            return None

    def save(self, name, data):
        if not self.directory:
            return
        destination = self.directory / name
        try:
            with tempfile.NamedTemporaryFile(dir=self.directory, prefix=".disc-", delete=False) as file:
                temporary = Path(file.name)
                try:
                    file.write(data)
                    file.flush()
                    os.fsync(file.fileno())
                    # Publish the complete file atomically, only if the name is still free.
                    os.link(temporary, destination)
                finally:
                    temporary.unlink()
        except FileExistsError:
            pass
        except OSError as error:
            print(f"Could not save cache entry {name}: {error}", file=sys.stderr)


class MusicBrainz:
    def __init__(self, disk):
        self.disk = disk
        self.cache = OrderedDict()
        self.cover_groups = OrderedDict()
        self.last_request = 0.0

    def lookup(self, disc, track_count):
        name = f"{disc}.txt"
        stored = self.disk.read(name, MAX_CACHE_TEXT)
        if stored:
            try:
                lines = stored.decode("utf-8").split("\n")
            except UnicodeError:
                lines = []
            if len(lines) == track_count + 6 and lines[0] == "OK" and lines[-1] == "":
                result = tuple(lines[:-2])
                release_id, group = result[-1], lines[-2]
                if not release_id or RELEASE_ID.fullmatch(release_id):
                    self.remember_cover_group(release_id, group)
                    print(f"Disk metadata cache hit: {disc}", flush=True)
                    return result
        if disc in self.cache:
            self.cache.move_to_end(disc)
            return self.cache[disc]

        query = urllib.parse.urlencode({"fmt": "json", "inc": "recordings+artists+artist-credits+release-groups"})
        request = urllib.request.Request(
            API + disc + "?" + query, headers={"User-Agent": USER_AGENT, "Accept": "application/json"}
        )
        group = ""
        for attempt in range(2):
            time.sleep(max(0.0, 1.05 - (time.monotonic() - self.last_request)))
            self.last_request = time.monotonic()
            try:
                with urllib.request.urlopen(request, timeout=6) as response:
                    data = response.read(8 * 1024 * 1024 + 1)
                    if len(data) > 8 * 1024 * 1024:
                        raise ValueError("oversized MusicBrainz response")
                payload = json.loads(data)
                result = select_matches(payload, disc, track_count)
                if result[0] == "OK" and result[-1]:
                    release_id = result[-1]
                    for release in payload.get("releases", []):
                        if release.get("id") != release_id:
                            continue
                        group = release.get("release-group", {}).get("id", "")
                        self.remember_cover_group(release_id, group)
                        break
            except urllib.error.HTTPError as error:
                if error.code == 404:
                    result = ("NO_MATCH",)
                elif attempt == 0 and error.code in (502, 503, 504):
                    continue
                else:
                    raise
            break
        if result[0] == "OK":
            self.disk.save(name, ("\n".join((*result, group)) + "\n").encode("utf-8"))
        self.cache[disc] = result
        if len(self.cache) > 128:
            self.cache.popitem(last=False)
        return result

    def remember_cover_group(self, release_id, group):
        if release_id and RELEASE_ID.fullmatch(group):
            self.cover_groups[release_id] = group
            self.cover_groups.move_to_end(release_id)
            if len(self.cover_groups) > 128:
                self.cover_groups.popitem(last=False)


def fetch_cover(path):
    request = urllib.request.Request(COVER + path + "/front-250", headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=5) as response:
        image = response.read(2 * 1024 * 1024 + 1)
    if len(image) > 2 * 1024 * 1024:
        raise ValueError("oversized cover image")
    return image


def cover_pixels(release_id, group_id=None):
    try:
        image = fetch_cover("release/" + release_id)
    except urllib.error.HTTPError as error:
        if error.code not in (404, 410) or not group_id:
            raise
        image = fetch_cover("release-group/" + group_id)
    if sys.platform != "darwin":
        from PIL import Image

        with Image.open(io.BytesIO(image)) as source:
            if source.width * source.height > 4096 * 4096:
                raise ValueError("oversized cover dimensions")
            with source.convert("RGB").resize((COVER_SIZE, COVER_SIZE), Image.Resampling.LANCZOS) as cover:
                return cover.tobytes("raw", "BGR")

    with tempfile.TemporaryDirectory() as folder:
        original = Path(folder) / "cover.jpg"
        bitmap = Path(folder) / "cover.bmp"
        original.write_bytes(image)
        subprocess.run(
            ["/usr/bin/sips", "-s", "format", "bmp", "-z", str(COVER_SIZE), str(COVER_SIZE),
             str(original), "--out", str(bitmap)],
            check=True, timeout=5, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        bmp = bitmap.read_bytes()
    if len(bmp) < 54 or bmp[:2] != b"BM":
        raise ValueError("invalid cover bitmap")
    offset = struct.unpack_from("<I", bmp, 10)[0]
    width, height, planes, depth, compression = struct.unpack_from("<iiHHI", bmp, 18)
    stride = COVER_SIZE * 3
    if (width != COVER_SIZE or abs(height) != COVER_SIZE or planes != 1 or depth != 24 or
            compression != 0 or offset < 54 or offset + stride * COVER_SIZE > len(bmp)):
        raise ValueError("unsupported cover bitmap")
    pixels = bmp[offset:offset + stride * COVER_SIZE]
    if height > 0:
        pixels = b"".join(pixels[row:row + stride] for row in range(len(pixels) - stride, -1, -stride))
    return pixels


class Handler(http.server.BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(10)

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        if parsed.path.startswith("/cover/"):
            self.send_cover(parsed.path.removeprefix("/cover/"))
            return
        if parsed.path != "/lookup":
            self.send_error(404)
            return
        fields = urllib.parse.parse_qs(parsed.query).get("toc", [])
        try:
            if len(fields) != 1 or len(fields[0]) > 1000:
                raise ValueError("expected one table of contents")
            parts = fields[0].split()
            if len(parts) < 4 or any(not part.isascii() or not part.isdecimal() for part in parts):
                raise ValueError("invalid table of contents")
            toc = tuple(map(int, parts))
            disc = disc_id(toc)
        except ValueError:
            self.send_error(400, "Invalid table of contents")
            return
        try:
            lines = self.server.musicbrainz.lookup(disc, toc[1] - toc[0] + 1)
        except (OSError, ValueError) as error:
            print(f"MusicBrainz request failed: {error}", file=sys.stderr)
            lines = ("UNAVAILABLE",)
        print(f"Metadata lookup {disc}: {lines[0]}", flush=True)
        body = ("\n".join(lines) + "\n").encode("utf-8")
        self.send_body(body, "text/plain; charset=utf-8")

    def send_cover(self, release_id):
        if not RELEASE_ID.fullmatch(release_id):
            self.send_error(404)
            return
        cache = self.server.covers
        if release_id not in cache:
            try:
                name = f"{release_id}.bgr"
                pixels = self.server.disk.read(name, COVER_BYTES)
                if pixels is not None and len(pixels) == COVER_BYTES:
                    print(f"Disk artwork cache hit: {release_id}", flush=True)
                else:
                    group_id = self.server.musicbrainz.cover_groups.get(release_id)
                    pixels = cover_pixels(release_id, group_id)
                    self.server.disk.save(name, pixels)
                cache[release_id] = pixels
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                print(f"Cover unavailable: {error}", file=sys.stderr)
                self.send_error(404)
                return
            if len(cache) > 24:
                cache.popitem(last=False)
        cache.move_to_end(release_id)
        self.send_body(cache[release_id], "application/octet-stream")

    def send_body(self, body, mime):
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, socket.timeout):
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", type=ipaddress.IPv4Address, default="127.0.0.1",
                        help="Local IPv4 address to listen on")
    parser.add_argument("--port", type=int, default=13660)
    parser.add_argument("--cache-dir", type=Path, help="Keep successful lookups and artwork on disk")
    args = parser.parse_args()
    if not (1 <= args.port <= 65535):
        parser.error("--port must be between 1 and 65535")
    with http.server.HTTPServer((str(args.bind), args.port), Handler) as server:
        server.disk = DiskCache(args.cache_dir)
        server.musicbrainz = MusicBrainz(server.disk)
        server.covers = OrderedDict()
        print(f"DiscCue 360 helper listening on {args.bind}:{args.port}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
