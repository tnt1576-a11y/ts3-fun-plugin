# Fun Plugin for TeamSpeak 3

A client-side plugin for messing with friends on your own TS3 server.

- **Random Move** — keep moving a chosen user into a random *empty* channel on a timer.
- **Make copies of myself** — enter how many extra connections to join, in ordinary TS3 tabs.
- **Talk Back** — stream a live pitched echo privately to one person, or back to everyone in your channel as normal voice.
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

Run `test.bat` for the offline SDK integration tests. They use fake connections and
an isolated temporary config directory; no server or real TeamSpeak settings are used.
When TS3 is installed in `C:\Program Files\TeamSpeak 3 Client`, it also tests the tab
bridge with that installation's Qt DLLs in a separate hidden test window, including
focus restoration, deferred tab disposal, and both a directly named tab control and
TeamSpeak's named-wrapper layout. It does not launch or connect TeamSpeak.
After rebuilding and packaging, `python tests/package_smoke.py` loads the exact DLL
from the installer and checks its version, lifecycle, menus, and native dialog resources.

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
- **Talk Back: Start... / Stop** — echo their voice as a live pitched whisper.

**Right-click a CHANNEL** (group actions on everyone in it):
- **Random Move / Poke / Chaos everyone here**
- **Make copies of myself here...** — choose the extra copy count and join this channel.
- **Talk Back: Channel...** — echo your current channel through your user so everyone there hears it.

**Global plugin menu:** **Settings...**, **HARD STOP - stop everything**,
**Self-lock: ON / OFF**, **Make copies of myself...**, and **Remove my copies (cancel queue)**.

### Make copies of myself

From your original connected tab, open **Plugins → Fun Plugin → Make copies of myself...**
to join your current channel, or right-click another channel and select **Make copies
of myself here...**. Enter the number of **extra** copies (5 means you plus 5 copies),
adjust the nickname prefix if desired, and supply the channel password when needed.

Copies appear as `YourName #1`, `YourName #2`, etc. Long and Unicode names are shortened
to fit TeamSpeak's nickname limit without splitting UTF-8 characters. Copies use your
**default TeamSpeak identity**, which can differ from the identity on your original tab.
Copies connect with capture and playback set to None, so they do not open your
microphone or speakers. Both are also muted. If TeamSpeak enables audio on a copy,
the queue stops and disconnects pending copies.
There is no microphone relay and no additional TeamSpeak process.

The count is any positive 32-bit whole number, with no fixed five-copy cap. Copies
connect one at a time, with a short settling pause after each connection establishes.
Your selected tab is preserved throughout joining. The actual
number is limited by server slots, channel capacity, identity clone limits and other
server permissions. The queue stops on a rejection, wrong channel, timeout, or loss
of the original connection, and reports how many copies joined. Successful copies
remain connected until removed; there is no automatic retry or limit bypass.

**Remove my copies (cancel queue)** cancels pending joins and disconnects all copies
made by this plugin session, leaving your original and unrelated connections alone.
Their tabs close automatically after disconnecting, through their normal close buttons.
Cleanup handles two tabs per GUI tick and retries deferred closes. **HARD STOP** and
unloading the plugin also remove those copies. Only tabs created during the current
plugin session are tracked; tabs left over from an earlier load need closing once.
Your original and unrelated tabs stay open. The count is remembered
in `funplugin.ini`; copies never join automatically, and passwords are not saved by
this feature.

Automatic tab management requires TS3's Windows Qt 5.15 runtime and the supported
server-tab widget. On an unsupported layout, the copy action reports that it cannot
start. Tabs left over from previous plugin loads are not adopted by name.

### Talk Back

Right-click someone in your current channel and select **Talk Back: Start...**.
Choose **Pitch down (deep)** or **Pitch up (high)**, an amount from 1 to 12 semitones,
and a volume from 10 to 100%. Once connected, the effect processes their voice
as it arrives and whispers the pitched echo back **only to that person**, while
they are still speaking. It keeps the original speaking pace. You must be able
to hear the speaker. There is no phrase recording or added replay delay; the
short audio-processing buffer and the network still add some latency.

Talk Back sends through **your existing user and microphone connection**. It
creates no extra user or tab and needs permission to whisper to the recipient.
Activate your microphone connection and unmute playback before starting it;
release any active whisper hotkey first. While enabled, the effect replaces
your microphone samples with the live echo, temporarily taking over mute,
push-to-talk, voice activation, and the active whisper route. Your own voice
is not mixed into the echo. Stop restores your original microphone settings
and normal channel routing. Saved whisper presets are not edited. Small audio
buffers stay in memory; the effect creates no recordings or voice files.

For the whole channel, choose **Talk Back: Channel...** in the global plugin menu
or right-click the channel you are in. Choose the same pitch and volume options.
Everyone else's audible channel voice is echoed live through **your user as normal
channel voice**, so everyone in that channel can hear it. Overlapping speakers are
mixed together at their original pace. People who join your channel are included
automatically. Your own voice and received private whispers are excluded. Channel
mode needs normal speaking permission; it does not need whisper permission.

Only one Talk Back mode can run at a time. **Talk Back: Stop** in the global plugin
menu and **HARD STOP** cancel either mode, discard buffered audio, and restore your
microphone. Private mode can also be stopped from the selected person's menu.
Leaving the channel, losing the connection, or manually muting also stops the
effect. Start it again in the new channel after moving. A rejected whisper stops
private mode, and activating a whisper hotkey stops channel mode.

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

HARD STOP is the failsafe — it cancels copy joins and disconnects copies, stops
the move/poke/chaos jobs (however they were started), **disarms every talk-poke**
target, and **turns off auto-poke-back**
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
