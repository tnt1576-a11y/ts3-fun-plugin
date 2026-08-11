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

**Right-click a USER:**
- **Random Move: Start / Stop**
- **Poke: Start / Stop**
- **Chaos (move + poke): Start / Stop** — both at once
- **Talk-poke: Arm / Disarm** — pokes them every time they start talking
- **Talk-move: Arm / Disarm** — moves them to another channel every time they start
  talking (follows your "no empty channel" setting, but never prompts mid-talk — it
  just skips the move if there's nowhere to send them)

**Right-click a CHANNEL** (group actions on everyone in it):
- **Random Move / Poke / Chaos everyone here**

**Global plugin menu:** **Settings...**, **HARD STOP - stop everything**, and
**Self-lock: ON / OFF**.

**Self-lock** protects *you*: while ON, if anyone force-moves or channel-kicks you out
of your channel, you snap right back. It also covers **server kicks** — if someone kicks
you off the server, the plugin reconnects and rejoins your channel automatically (it
captures your server address/password/nickname when you enable self-lock). You can still
switch channels yourself (that re-anchors the lock), and a disconnect *you* initiate
won't reconnect (only an actual kick does). A **ban** can't be undone this way. Turn it
OFF from the same menu. Session-only and unaffected by HARD STOP.

**Talk reaction delay:** talk-poke and talk-move wait a configurable delay (default 1s,
Settings > Reactions) before firing, so the person gets a word out before it hits. Set
it to 0 for instant.

HARD STOP is the failsafe — it stops **all** activity: the move/poke/chaos jobs (however
they were started), **disarms every talk-poke** target, and **turns off auto-poke-back**
for the session (re-enable it in Settings). The individual *Stop* items only stop their
own job and leave the reactive features alone.

Open **Settings...** for the move/poke intervals, message mode, and:
- **Auto-poke-back** — when someone pokes you, poke them back N times (5s per-person
  cooldown so two auto-pokers don't ping-pong forever).
- **Talk-poke pokes per talk** — how many pokes each talk trigger sends.
- **Auto-stop limiter** — run for N seconds then stop by itself (0 = no limit).

### "No empty channel" behaviour
- **Ask me each time** — if nothing is empty when you start a move, you get a Yes/No
  prompt; Yes = move among *all* channels, No = cancel.
- **Use ALL channels** — always allow non-empty channels as a fallback.
- **Skip until one frees up** — only ever move into truly empty channels.
- **Auto-create temporary empty channels** — if on, the plugin spawns throwaway empty
  channels to move the victim into (they self-delete when empty).

### Very fast timing (below 0.1s)
The interval fields accept decimals down to `0.01` (10 ms). **But** the server's
anti-flood protection will start blocking commands — and can briefly kick you — if you
fire moves/pokes too fast. To actually use ultra-fast speeds, grant your group the
`b_client_ignore_antiflood` permission on your server (you're the owner). Otherwise keep
intervals around `0.3s`+ to stay safe.

## Notes
- You are the server owner, so channel-move / channel-create permissions are assumed.
- The plugin API version must match your client; if the client rejects the plugin,
  update `PLUGIN_API_VERSION` (comes from the SDK's `plugin.h`) and rebuild.
