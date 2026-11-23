# DiscCue 360

DiscCue 360 plays physical audio CDs on an RGH Xbox 360. It reads the disc when you insert it and uses a LAN helper to look up track names and cover art from MusicBrainz and Cover Art Archive. Playback works without a network connection or Xbox Live.

## Build and run

1. Set up the [xsdk toolchain](https://github.com/xenosworks/xsdk/blob/9e6de98f6b5d737bce0fecd2864edae94bdd2637/docs/GETTING_STARTED.md) on Linux or WSL, then build the player.

   ```sh
   cmake -S . -B build -G Ninja \
     -DCMAKE_TOOLCHAIN_FILE="$XSDK_ROOT/cmake/xsdk.cmake" \
     -DXSDK_TOOLCHAIN="$XSDK_TOOLCHAIN"
   cmake --build build --target default
   ```

2. Copy `build/default.xex` to `Hdd1:\Apps\DiscCue360\` and open it in XEXMenu.

## Track names and artwork

Run the Python 3 helper on another computer on the same LAN. Artwork conversion uses `sips` on macOS and Pillow on Linux.

```sh
python3 helper.py --bind "$HELPER_LAN_IP" --cache-dir ./cache
```

Set `HELPER_LAN_IP` to the helper computer’s LAN IPv4 address. Put that address on one line in `helper-ip.txt` next to `default.xex`. The Xbox connects to the helper on TCP port 13660. Successful lookups and downloaded artwork are saved in the cache directory, and existing cache files are preserved.

## Controls

| Button | Action |
| --- | --- |
| Left stick / D-pad | Select a track |
| A | Play, pause or resume |
| X | Stop |
| Y | Toggle repeat |
| LB | Open or close the tray |
| B / Back | Exit |

DiscCue supports audio-only CDs. Metadata availability depends on the pressing. The experimental `PlayCD` build target is separate from the player and remains unverified on the stock dashboard.
