# Deploy the LAN helper on Linux

The example [systemd user service](disccue-helper.service) runs the DiscCue helper on TCP port `13660`. It saves successful lookups and artwork under `~/.local/state/disccue360/cache`.

1. Install Python 3 and Pillow. On Debian or Ubuntu:

   ```sh
   sudo apt install python3 python3-pil
   ```

2. From the repository root, set `HELPER_LAN_IP` to your server’s LAN IPv4 address and install the service:

   ```sh
   install -Dm 644 helper.py ~/.local/share/disccue360/helper.py
   install -Dm 644 deploy/disccue-helper.service ~/.config/systemd/user/disccue-helper.service
   mkdir -p ~/.config/disccue360
   printf 'DISCCUE_BIND=%s\n' "$HELPER_LAN_IP" > ~/.config/disccue360/helper.env
   systemctl --user daemon-reload
   systemctl --user enable --now disccue-helper.service
   ```

3. To start it at boot without logging in, enable lingering:

   ```sh
   sudo loginctl enable-linger "$USER"
   ```

4. Put the server’s IPv4 address in `helper-ip.txt` beside DiscCue’s `default.xex` on the Xbox. If your server firewall filters LAN connections, allow TCP `13660` from the Xbox. Insert an audio CD and watch for the lookup:

   ```sh
   journalctl --user -u disccue-helper.service -f
   ```

## Use an HTTP proxy

Add these lines to `~/.config/disccue360/helper.env`, replacing the address and port with your proxy’s. Requests to MusicBrainz and Cover Art Archive use this proxy; requests from the Xbox go directly to the LAN helper.

```ini
http_proxy=http://127.0.0.1:8080
https_proxy=http://127.0.0.1:8080
no_proxy=
```

Apply the change with `systemctl --user restart disccue-helper.service`.
