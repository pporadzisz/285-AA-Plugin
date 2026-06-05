# 285AApanel

An XFCE4 panel plugin that displays game server statistics: Players Online | Players In-Game | Servers.

The plugin fetches data from:
- Players Online and Servers: Parsed from a server list URL.
- Players In-Game: Parsed from a server list URL.

Features:
- Displays statistics in the panel with green text.
- Tooltip shows detailed server information, sorted by players in-game.
- Configurable refresh interval and click command via properties dialog.
- Click the plugin to run a custom command (e.g., launch game).
- Settings persistence across sessions.
- Automatic panel restart on install/remove.

## Dependencies

- xfce4-panel
- libxfce4panel-2.0-4
- libgtk-3-0
- libcurl4
- libnotify4

## Building from Source

Ensure dependencies are installed:

```bash
sudo apt install libxfce4panel-2.0-dev libgtk-3-dev libcurl4-openssl-dev libnotify-dev libjson-glib-dev
```

Compile the plugin:

```bash
make
```

This produces `285AApanel.so`.

## Building the .deb Package

Copy the compiled plugin to the deb structure:

```bash
cp 285AApanel.so deb/usr/lib/xfce4/panel-plugins/lib285AApanel
```

Build the Debian package:

```bash
dpkg-deb -b deb 285AApanel_1.0_amd64.deb
```

## Installation

Install the .deb package:

```bash
sudo dpkg -i 285AApanel_1.0_amd64.deb
```

The postinst script will install the plugin and restart the XFCE panel automatically.

## Manual Installation

If not using the .deb, copy files manually:

```bash
sudo cp 285AApanel.so /usr/lib/xfce4/panel-plugins/lib285AApanel.so
sudo cp 285AApanel.desktop /usr/share/xfce4/panel/plugins/
sudo ln -sf /usr/lib/xfce4/panel-plugins/lib285AApanel.so /usr/lib/x86_64-linux-gnu/xfce4/panel/plugins/lib285AApanel.so
```

Restart the XFCE panel:

```bash
xfce4-panel -r
```

## Installation from GitHub

You can install the prebuilt `.deb` package directly from a GitHub Release.

Replace `<owner>`, `<repo>` and `<tag>` with the repository owner, repository name and release tag respectively.

Download a specific release asset and install:

```bash
TAG=v1.0.0
wget -O 285AApanel_${TAG}_amd64.deb "https://github.com/<owner>/<repo>/releases/download/${TAG}/285AApanel_${TAG}_amd64.deb"
sudo dpkg -i 285AApanel_${TAG}_amd64.deb
sudo apt-get install -f   # fix missing dependencies if needed
```

Download the latest release (requires `jq`):

```bash
LATEST_URL=$(curl -s https://api.github.com/repos/<owner>/<repo>/releases/latest | jq -r '.assets[] | select(.name|test("285AApanel_.*_amd64.deb")) | .browser_download_url')
wget -O 285AApanel_latest.deb "$LATEST_URL"
sudo dpkg -i 285AApanel_latest.deb
sudo apt-get install -f
```

Or visit the Releases page in your browser and download the `.deb` asset manually:

https://github.com/<owner>/<repo>/releases

After installation restart the XFCE panel:

```bash
xfce4-panel -r
```

## Uninstallation

Remove the package:

```bash
sudo dpkg -r 285aapanel
```

Or manually:

```bash
sudo rm /usr/lib/xfce4/panel-plugins/lib285AApanel.so
sudo rm /usr/lib/x86_64-linux-gnu/xfce4/panel/plugins/lib285AApanel.so
sudo rm /usr/share/xfce4/panel/plugins/285AApanel.desktop
```

Restart the XFCE panel:

```bash
xfce4-panel -r
```

## Usage

Add the "285AApanel" plugin to the XFCE panel via Panel Preferences > Items > Add.

Right-click the plugin > Properties to configure settings:
- Refresh Interval (seconds)
- Command (run on click)
- Enable Notifications
- Minimum Players for Notification
- Mission Name for Notification

The plugin displays: Online | In-Game | Servers.

Tooltip shows server details.

Notifications trigger when a mission has at least the minimum players.

```bash
xfce4-panel -r
```

## Installation from .deb Package

A .deb package is available for easy installation.

Build the package:

```bash
dpkg-deb --build deb 285AApanel_1.0_amd64.deb
```

Install:

```bash
sudo dpkg -i 285AApanel_1.0_amd64.deb
```

Uninstall:

```bash
sudo dpkg -r 285AApanel
```

## Usage

1. Add the "285AApanel" plugin to your XFCE panel via panel preferences.
2. The plugin displays "Online | In-Game | Servers".
3. Hover for tooltip with server details.
4. Right-click for properties to configure refresh interval and command.
5. Left-click to execute the configured command.

## Configuration

Right-click the plugin and select "Properties" to open the config file in your default editor.

Edit the following settings in `~/.config/xfce4/panel/285AApanel/config`:

- **refresh_interval**: Time in seconds between data updates (default: 60).
- **command**: Executable command run on left-click (e.g., `steam steam://rungameid/123`).
- **notify_enabled**: true/false for desktop notifications.
- **min_players**: Minimum players for notification trigger (default: 3).
- **mission_name**: Mission name substring (case-insensitive) for notifications (default: Pipeline).

Changes take effect on the next data update.

## Customization

Modify `285AApanel.c` for data source changes. Update `deb/DEBIAN/control` for package metadata.

All in one command:

make && cp 285AApanel.so deb/usr/lib/xfce4/panel-plugins/lib285AApanel && dpkg-deb -b deb 285AApanel_1.0_amd64.deb && sudo dpkg -i 285AApanel_1.0_amd64.deb

## Recent changes (developer notes)

- Added `online_users` column (leftmost) fetched from the auth ping JSON endpoint
- Data fetching moved to a background thread to avoid UI blocking; results are applied on the GTK main thread.
- Plugin now counts only servers with `query_result.success == true` when computing totals.
- Added runtime logging to `/tmp/285AApanel.log` to help diagnose crashes and state changes.
- Fixed a crash caused by calling `curl_global_cleanup()` and `notify_uninit()` while background threads were running. Global cleanup is no longer invoked during `free_data()`.

## Debugging & troubleshooting

- If the plugin disappears from the panel and XFCE shows the "Plugin unexpectedly left the panel" dialog, check the plugin log:
```bash
tail -f /tmp/285AApanel.log
```
- The log contains timestamps and messages about construction, fetch results, idle scheduling, and label updates.
- If you see frequent construct/free cycles, the panel is repeatedly removing and re-adding the plugin. Restarting the panel (`xfce4-panel --restart`) often stabilizes it after updates.
- To capture a crash backtrace, stop the running panel and run it under `gdb` (this will interrupt your desktop panel temporarily):
```bash
xfce4-panel --quit
gdb --args xfce4-panel
# then in gdb: run
# after crash: bt full
```

If you want help collecting logs or running under gdb, open an issue or ask for assistance.