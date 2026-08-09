# Fun Plugin for TeamSpeak 3

A client-side plugin for messing with friends on your own TS3 server.

- **Random Move** — keep moving a chosen user into a random *empty* channel on a timer.
- **Poke** — keep poking a chosen user with a custom or random message on a timer.
- **HARD STOP** — one click halts everything.
- **Settings** — native dialog for intervals and messages (saved to `funplugin.ini`).

Target: 64-bit TS3 client (`ts3client_win64.exe`).

## Build

Prerequisites (placed inside this project):
- `tools/w64devkit/` — portable MinGW-w64 toolchain (provides `g++`, `windres`).
- `sdk/include/` — TeamSpeak 3 Plugin SDK headers (`plugin.h`, `ts3_functions.h`, `teamspeak/*.h`).

Then run:

```bash
build.bat
```

This produces `funplugin_win64.dll`.

## Install

### Easy: the packaged installer (recommended)
Double-click **`dist/ts3-fun-plugin.ts3_plugin`**. TeamSpeak shows its
"TeamSpeak Add-On" dialog — click **Install**, then enable it under
**Tools → Options → Addons**.

Rebuild the package after changing the DLL:

```bash
python make_package.py
```

A `.ts3_plugin` file is just a ZIP containing `package.ini` (the metadata shown in
the install dialog) and `plugins/funplugin_win64.dll`.

### Manual: copy the DLL
Copy the DLL to your client's plugin folder:

```
C:\Users\<you>\AppData\Roaming\TS3Client\plugins\
```

In TeamSpeak: **Tools → Options → Addons**, reload the list, enable **Fun Plugin**
(or just restart the client).

## Use

1. Right-click a user in the channel tree → you'll see:
   - **Random Move: Start / Stop**
   - **Poke: Start / Stop**
2. The global plugin menu has **Settings...** and **HARD STOP - stop everything**.
3. Open **Settings...** to set the move interval, poke delay, custom/random messages,
   and how the mover behaves when no channel is empty.

### "No empty channel" behaviour
- **Ask me each time** — if nothing is empty when you start a move, you get a Yes/No
  prompt; Yes = move among *all* channels, No = cancel.
- **Use ALL channels** — always allow non-empty channels as a fallback.
- **Skip until one frees up** — only ever move into truly empty channels.
- **Auto-create temporary empty channels** — if on, the plugin spawns throwaway empty
  channels to move the victim into (they self-delete when empty).

## Notes
- You are the server owner, so channel-move / channel-create permissions are assumed.
- The plugin API version must match your client; if the client rejects the plugin,
  update `PLUGIN_API_VERSION` (comes from the SDK's `plugin.h`) and rebuild.
