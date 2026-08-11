/*
 * Fun Plugin for TeamSpeak 3 (client-side, 64-bit)
 *
 * Features, driven from the right-click menus:
 *   - Random Move  : keep moving target(s) into random empty channels on a timer.
 *   - Poke         : keep poking target(s) with a custom/random/gibberish message.
 *   - Chaos        : move + poke at once.
 *   - Group actions: right-click a CHANNEL to hit everyone in it at once.
 *   - Talk-poke    : arm a user so they get poked whenever they start talking.
 *   - Auto-poke-back: (setting) poke people back automatically when they poke you.
 *   - Limiter      : auto-stop after N seconds.
 *   - HARD STOP    : global panic button.
 *
 * For your own server where you are the owner (permissions are not an issue).
 */

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <utility>

#include "teamspeak/public_errors.h"
#include "teamspeak/public_definitions.h"
#include "ts3_functions.h"
#include "plugin.h"

#include "resource.h"

/* The SDK header declares ts3plugin_apiVersion() but does not define the number;
 * the demo plugin defines it itself. 26 matches modern TS3 clients (3.5/3.6). */
#ifndef PLUGIN_API_VERSION
#define PLUGIN_API_VERSION 26
#endif

/* ------------------------------------------------------------------ */
/* Globals                                                            */
/* ------------------------------------------------------------------ */

static struct TS3Functions ts3Functions;   /* filled in by setFunctionPointers */
static char*     pluginID  = NULL;          /* our registered plugin id         */
static HINSTANCE hInstance = NULL;          /* DLL instance (for the dialog)     */

#define PLUGIN_NAME    "Fun Plugin"
#define PLUGIN_VERSION "2.1"
#define PLUGIN_AUTHOR  "kingtnt"
#define PLUGIN_DESC    "Random-move, poke and chaos your friends for fun."

/* Menu item ids */
enum {
    MENU_MOVE_START = 1,
    MENU_MOVE_STOP,
    MENU_POKE_START,
    MENU_POKE_STOP,
    MENU_CHAOS_START,
    MENU_CHAOS_STOP,
    MENU_TALKPOKE_START,
    MENU_TALKPOKE_STOP,
    MENU_TALKMOVE_START,
    MENU_TALKMOVE_STOP,
    MENU_MOVE_START_CH,   /* channel menu: everyone in the channel */
    MENU_POKE_START_CH,
    MENU_CHAOS_START_CH,
    MENU_SETTINGS,
    MENU_HARDSTOP,
    MENU_SELFLOCK_ON,     /* global: lock myself into my current channel */
    MENU_SELFLOCK_OFF
};

/* "No empty channel" behaviour */
enum { NE_ASK = 0, NE_ALL = 1, NE_SKIP = 2 };
/* Poke message source */
enum { PM_CUSTOM = 0, PM_RANDOM = 1, PM_RANDCHARS = 2 };

/* ------------------------------------------------------------------ */
/* Settings                                                           */
/* ------------------------------------------------------------------ */

struct Settings {
    int  moveIntervalMs;   /* time between moves (ms), supports sub-second */
    int  noEmptyMode;      /* NE_ASK / NE_ALL / NE_SKIP  */
    int  autoCreateTemp;   /* 0/1                        */
    int  pokeDelayMs;      /* time between pokes (ms)    */
    int  pokeMode;         /* PM_CUSTOM / PM_RANDOM / PM_RANDCHARS */
    int  randCharLen;      /* length for PM_RANDCHARS    */
    int  limiterSec;       /* auto-stop after N seconds; 0 = no limit */
    int  autoPokeBack;     /* 0/1: poke back when poked  */
    int  autoPokeBackTimes;/* pokes per auto-poke-back   */
    int  talkPokeTimes;    /* pokes each time a watched user starts talking */
    int  talkDelayMs;      /* delay before a talk reaction fires; 0 = instant */
    int  selfLockOn;       /* 0/1: persisted; auto-arms self-lock on every connect */
    char customMsg[256];
    char talkPokeMsg[256]; /* dedicated talk-poke text; blank = use poke settings */
    std::vector<std::string> randomMsgs;
};

static Settings g;
static CRITICAL_SECTION cs;   /* guards job start/stop + shared reaction state */

/* Reaction state (guarded by cs) */
static std::set<anyID>          talkTargets;      /* users armed for talk-poke  */
static std::set<anyID>          talkMoveTargets;  /* users armed for talk-move  */
static std::map<anyID, DWORD>   pokeBackCooldown; /* last auto-poke-back tick    */
static std::map<anyID, DWORD>   talkCooldown;     /* last talk-poke tick         */
static std::map<anyID, DWORD>   talkMoveCooldown; /* last talk-move tick         */

/* Self-lock: pin my own client into a channel (guarded by cs) */
static volatile LONG selfLock = 0;
static uint64        selfLockSch = 0;
static uint64        selfLockChannel = 0;

/* Server-kick auto-rejoin (part of self-lock). g_reconn guarded by cs. */
struct ReconnectInfo {
    bool valid;
    char address[256];      /* "host:port"        */
    char password[256];     /* server password    */
    char nickname[128];
    char serverName[128];   /* used to match the bookmark */
    char channelPath[512];  /* "Parent/Child/..." */
};
static ReconnectInfo g_reconn = {};
static volatile LONG g_rejoinAttempts = 0;   /* capped so a ban / dead server can't loop forever */
static volatile LONG g_myId = 0;             /* my client id on the locked connection */
static volatile LONG g_kickFlag = 0;         /* set when I was kicked from the SERVER */

/* Delayed-reaction plumbing (so talk reactions can wait a beat) */
static HANDLE        g_shutdownEvent = NULL;   /* signalled on shutdown to cancel pending */
static volatile LONG g_pending = 0;            /* outstanding delayed-reaction threads    */

static void setDefaults() {
    g.moveIntervalMs   = 5000;
    g.noEmptyMode      = NE_ASK;
    g.autoCreateTemp   = 0;
    g.pokeDelayMs      = 3000;
    g.pokeMode         = PM_CUSTOM;
    g.randCharLen      = 8;
    g.limiterSec       = 0;
    g.autoPokeBack     = 0;
    g.autoPokeBackTimes= 3;
    g.talkPokeTimes    = 1;
    g.talkDelayMs      = 1000;   /* 1s so the victim gets a word out first */
    g.selfLockOn       = 0;
    strcpy(g.customMsg, "stop ignoring me!");
    g.talkPokeMsg[0]   = '\0';   /* blank = fall back to poke settings */
    g.randomMsgs.clear();
    g.randomMsgs.push_back("boo!");
    g.randomMsgs.push_back("wake up");
    g.randomMsgs.push_back("hehe");
    g.randomMsgs.push_back("gotcha");
}

/* Config file lives next to the TS3 client config. */
static void getIniPath(char* out, size_t n) {
    char cfg[512];
    cfg[0] = '\0';
    ts3Functions.getConfigPath(cfg, sizeof(cfg));   /* ends with a path separator */
    _snprintf(out, n, "%sfunplugin.ini", cfg);
    out[n - 1] = '\0';
}

static void loadSettings() {
    setDefaults();
    char ini[600];
    getIniPath(ini, sizeof(ini));
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) return;  /* keep defaults */

    g.moveIntervalMs   = GetPrivateProfileIntA("move", "intervalMs", g.moveIntervalMs, ini);
    g.noEmptyMode      = GetPrivateProfileIntA("move", "noEmptyMode", g.noEmptyMode, ini);
    g.autoCreateTemp   = GetPrivateProfileIntA("move", "autoCreate",  g.autoCreateTemp, ini);
    g.pokeDelayMs      = GetPrivateProfileIntA("poke", "delayMs",    g.pokeDelayMs, ini);
    g.pokeMode         = GetPrivateProfileIntA("poke", "mode",        g.pokeMode, ini);
    g.randCharLen      = GetPrivateProfileIntA("poke", "randLen",    g.randCharLen, ini);
    g.limiterSec       = GetPrivateProfileIntA("limiter", "seconds", g.limiterSec, ini);
    g.autoPokeBack     = GetPrivateProfileIntA("react", "autoPokeBack",  g.autoPokeBack, ini);
    g.autoPokeBackTimes= GetPrivateProfileIntA("react", "autoPokeBackTimes", g.autoPokeBackTimes, ini);
    g.talkPokeTimes    = GetPrivateProfileIntA("react", "talkPokeTimes", g.talkPokeTimes, ini);
    g.talkDelayMs      = GetPrivateProfileIntA("react", "talkDelayMs",   g.talkDelayMs, ini);
    g.selfLockOn       = GetPrivateProfileIntA("selflock", "on",        g.selfLockOn, ini);
    GetPrivateProfileStringA("poke", "custom", g.customMsg, g.customMsg, sizeof(g.customMsg), ini);
    GetPrivateProfileStringA("react", "talkPokeMsg", g.talkPokeMsg, g.talkPokeMsg, sizeof(g.talkPokeMsg), ini);

    int count = GetPrivateProfileIntA("poke", "randomCount", -1, ini);
    if (count >= 0) {
        g.randomMsgs.clear();
        for (int i = 0; i < count; ++i) {
            char key[32], buf[256];
            _snprintf(key, sizeof(key), "random%d", i);
            GetPrivateProfileStringA("poke", key, "", buf, sizeof(buf), ini);
            if (buf[0]) g.randomMsgs.push_back(buf);
        }
    }
}

static void saveSettings() {
    char ini[600];
    getIniPath(ini, sizeof(ini));
    char num[32];

    _snprintf(num, sizeof(num), "%d", g.moveIntervalMs);
    WritePrivateProfileStringA("move", "intervalMs", num, ini);
    _snprintf(num, sizeof(num), "%d", g.noEmptyMode);
    WritePrivateProfileStringA("move", "noEmptyMode", num, ini);
    _snprintf(num, sizeof(num), "%d", g.autoCreateTemp);
    WritePrivateProfileStringA("move", "autoCreate", num, ini);

    _snprintf(num, sizeof(num), "%d", g.pokeDelayMs);
    WritePrivateProfileStringA("poke", "delayMs", num, ini);
    _snprintf(num, sizeof(num), "%d", g.pokeMode);
    WritePrivateProfileStringA("poke", "mode", num, ini);
    _snprintf(num, sizeof(num), "%d", g.randCharLen);
    WritePrivateProfileStringA("poke", "randLen", num, ini);
    WritePrivateProfileStringA("poke", "custom", g.customMsg, ini);

    _snprintf(num, sizeof(num), "%d", g.limiterSec);
    WritePrivateProfileStringA("limiter", "seconds", num, ini);

    _snprintf(num, sizeof(num), "%d", g.autoPokeBack);
    WritePrivateProfileStringA("react", "autoPokeBack", num, ini);
    _snprintf(num, sizeof(num), "%d", g.autoPokeBackTimes);
    WritePrivateProfileStringA("react", "autoPokeBackTimes", num, ini);
    _snprintf(num, sizeof(num), "%d", g.talkPokeTimes);
    WritePrivateProfileStringA("react", "talkPokeTimes", num, ini);
    _snprintf(num, sizeof(num), "%d", g.talkDelayMs);
    WritePrivateProfileStringA("react", "talkDelayMs", num, ini);
    WritePrivateProfileStringA("react", "talkPokeMsg", g.talkPokeMsg, ini);

    _snprintf(num, sizeof(num), "%d", g.selfLockOn);
    WritePrivateProfileStringA("selflock", "on", num, ini);

    _snprintf(num, sizeof(num), "%d", (int)g.randomMsgs.size());
    WritePrivateProfileStringA("poke", "randomCount", num, ini);
    for (size_t i = 0; i < g.randomMsgs.size(); ++i) {
        char key[32];
        _snprintf(key, sizeof(key), "random%d", (int)i);
        WritePrivateProfileStringA("poke", key, g.randomMsgs[i].c_str(), ini);
    }
}

/* ------------------------------------------------------------------ */
/* Message helpers                                                    */
/* ------------------------------------------------------------------ */

static std::string randomChars(int n) {
    static const char cs_[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    if (n < 1)   n = 1;
    if (n > 100) n = 100;
    std::string s;
    s.reserve(n);
    for (int i = 0; i < n; ++i) s += cs_[rand() % (sizeof(cs_) - 1)];
    return s;
}

static std::string pickPokeMessage() {
    if (g.pokeMode == PM_RANDOM && !g.randomMsgs.empty())
        return g.randomMsgs[rand() % g.randomMsgs.size()];
    if (g.pokeMode == PM_RANDCHARS)
        return randomChars(g.randCharLen);
    return g.customMsg;
}

static void pokeTimes(uint64 sch, anyID target, int times) {
    for (int i = 0; i < times; ++i) {
        std::string m = pickPokeMessage();
        ts3Functions.requestClientPoke(sch, target, m.c_str(), NULL);
    }
}

/* poke a fixed message N times (used by talk-poke's dedicated text) */
static void pokeTimesMsg(uint64 sch, anyID target, int times, const char* msg) {
    for (int i = 0; i < times; ++i)
        ts3Functions.requestClientPoke(sch, target, msg, NULL);
}

/* ------------------------------------------------------------------ */
/* Job engine (multi-target)                                          */
/* ------------------------------------------------------------------ */

struct Job {
    volatile LONG active;
    HANDLE  thread;
    HANDLE  stopEvent;         /* manual-reset; signalled to stop    */
    uint64  sch;               /* server connection handler id       */
    std::vector<anyID> targets;/* one or many victims                */
    int     allChannels;       /* move: 1 = may use non-empty channels */
    int     intervalMs;        /* snapshot of the interval for this run */
    int     limiterMs;         /* snapshot of auto-stop time; 0 = none  */
};

static Job moveJob;
static Job pokeJob;

static void stopJob(Job& j) {
    if (j.stopEvent) SetEvent(j.stopEvent);
    if (j.thread) {
        WaitForSingleObject(j.thread, 5000);
        CloseHandle(j.thread);
        j.thread = NULL;
    }
    if (j.stopEvent) {
        CloseHandle(j.stopEvent);
        j.stopEvent = NULL;
    }
    InterlockedExchange(&j.active, 0);
}

/* create the stop event + worker thread (caller must have stopped any prior run) */
static void launchJob(Job& j, LPTHREAD_START_ROUTINE fn) {
    j.stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    InterlockedExchange(&j.active, 1);
    j.thread = CreateThread(NULL, 0, fn, &j, 0, NULL);
}

/* interruptible sleep; returns true if we were told to stop */
static bool sleepOrStop(Job* j, int ms) {
    if (ms < 5) ms = 5;   /* backstop; the real floor is applied on input */
    return WaitForSingleObject(j->stopEvent, ms) == WAIT_OBJECT_0;
}

/* true once the run has hit its auto-stop limit */
static bool limitReached(Job* j, DWORD startTick) {
    return j->limiterMs > 0 && (DWORD)(GetTickCount() - startTick) >= (DWORD)j->limiterMs;
}

/* ------------------------------------------------------------------ */
/* Channel / client helpers                                           */
/* ------------------------------------------------------------------ */

static anyID ownClient(uint64 sch) {
    anyID me = 0;
    ts3Functions.getClientID(sch, &me);
    return me;
}

/* clients in a channel, excluding ourselves */
static std::vector<anyID> clientsInChannel(uint64 sch, uint64 channelID) {
    std::vector<anyID> v;
    anyID me = ownClient(sch);
    anyID* arr = NULL;
    if (ts3Functions.getChannelClientList(sch, channelID, &arr) == ERROR_ok && arr) {
        for (int i = 0; arr[i]; ++i)
            if (arr[i] != me) v.push_back(arr[i]);
        ts3Functions.freeMemory(arr);
    }
    return v;
}

/* true if at least one channel on the server currently has zero clients */
static bool hasAnyEmptyChannel(uint64 sch) {
    uint64* channels = NULL;
    if (ts3Functions.getChannelList(sch, &channels) != ERROR_ok || !channels) return false;
    bool found = false;
    for (int i = 0; channels[i] && !found; ++i) {
        anyID* cl = NULL;
        if (ts3Functions.getChannelClientList(sch, channels[i], &cl) == ERROR_ok && cl) {
            if (cl[0] == 0) found = true;
            ts3Functions.freeMemory(cl);
        }
    }
    ts3Functions.freeMemory(channels);
    return found;
}

/* Best-effort: ask the server to create a temporary empty channel at root.
 * We do not need its id - the next move cycle will simply find it empty. */
static void createTempChannel(uint64 sch) {
    static int counter = 0;
    char name[64];
    _snprintf(name, sizeof(name), "funbox-%d-%u", ++counter, (unsigned)(GetTickCount() & 0xffff));

    ts3Functions.setChannelVariableAsString(sch, 0, CHANNEL_NAME, name);
    ts3Functions.setChannelVariableAsInt(sch, 0, CHANNEL_FLAG_PERMANENT, 0);
    ts3Functions.setChannelVariableAsInt(sch, 0, CHANNEL_FLAG_SEMI_PERMANENT, 0);
    ts3Functions.setChannelVariableAsInt(sch, 0, CHANNEL_DELETE_DELAY, 60);
    ts3Functions.flushChannelCreation(sch, 0, NULL);   /* parent 0 = root */
}

static void doOneMove(Job* j) {
    uint64* channels = NULL;
    if (ts3Functions.getChannelList(j->sch, &channels) != ERROR_ok || !channels) return;

    std::vector<uint64> empties;  /* consumed as we go so a group spreads out */
    std::vector<uint64> all;
    for (int i = 0; channels[i]; ++i) {
        uint64 ch = channels[i];
        all.push_back(ch);
        anyID* cl = NULL;
        if (ts3Functions.getChannelClientList(j->sch, ch, &cl) == ERROR_ok && cl) {
            if (cl[0] == 0) empties.push_back(ch);
            ts3Functions.freeMemory(cl);
        }
    }
    ts3Functions.freeMemory(channels);

    bool createdThisTick = false;
    for (size_t t = 0; t < j->targets.size(); ++t) {
        anyID target = j->targets[t];
        uint64 cur = 0;
        ts3Functions.getChannelOfClient(j->sch, target, &cur);

        uint64 dest = 0;
        int pick = -1;
        if (!empties.empty()) {
            for (int tries = 0; tries < (int)empties.size(); ++tries) {
                int idx = rand() % (int)empties.size();
                if (empties[idx] != cur) { pick = idx; break; }
            }
        }
        if (pick >= 0) {
            dest = empties[pick];
            empties.erase(empties.begin() + pick);      /* don't reuse for this tick */
        } else if (j->allChannels) {
            for (int tries = 0; tries < 8 && !all.empty(); ++tries) {
                uint64 c = all[rand() % (int)all.size()];
                if (c != cur) { dest = c; break; }
            }
        } else if (g.autoCreateTemp && !createdThisTick) {
            createTempChannel(j->sch);                  /* used automatically next tick */
            createdThisTick = true;
            continue;
        } else {
            continue;                                   /* skip this target this cycle */
        }

        if (dest) ts3Functions.requestClientMove(j->sch, target, dest, "", NULL);
    }
}

static void doOnePoke(Job* j) {
    for (size_t i = 0; i < j->targets.size(); ++i) {
        std::string m = pickPokeMessage();
        ts3Functions.requestClientPoke(j->sch, j->targets[i], m.c_str(), NULL);
    }
}

static DWORD WINAPI moveThread(LPVOID p) {
    Job* j = (Job*)p;
    srand((unsigned)time(NULL) ^ (unsigned)(uintptr_t)j);
    DWORD start = GetTickCount();
    for (;;) {
        doOneMove(j);
        if (limitReached(j, start)) break;
        if (sleepOrStop(j, j->intervalMs)) break;
        if (limitReached(j, start)) break;
    }
    InterlockedExchange(&j->active, 0);
    return 0;
}

static DWORD WINAPI pokeThread(LPVOID p) {
    Job* j = (Job*)p;
    srand((unsigned)time(NULL) ^ (unsigned)(uintptr_t)j);
    DWORD start = GetTickCount();
    for (;;) {
        doOnePoke(j);
        if (limitReached(j, start)) break;
        if (sleepOrStop(j, j->intervalMs)) break;
        if (limitReached(j, start)) break;
    }
    InterlockedExchange(&j->active, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Start / stop actions                                               */
/* ------------------------------------------------------------------ */

static void startMove(uint64 sch, const std::vector<anyID>& targets) {
    if (targets.empty()) return;

    int allMode = (g.noEmptyMode == NE_ALL) ? 1 : 0;
    if (g.noEmptyMode == NE_ASK && !hasAnyEmptyChannel(sch)) {
        int r = MessageBoxA(GetForegroundWindow(),
                            "No empty channels are available right now.\n\n"
                            "Move among ALL channels anyway?",
                            "Random Move", MB_YESNO | MB_ICONWARNING);
        if (r == IDYES) allMode = 1;
        else return;
    }

    EnterCriticalSection(&cs);
    stopJob(moveJob);
    moveJob.sch = sch;
    moveJob.targets = targets;
    moveJob.allChannels = allMode;
    moveJob.intervalMs = g.moveIntervalMs;
    moveJob.limiterMs = g.limiterSec * 1000;
    launchJob(moveJob, moveThread);
    LeaveCriticalSection(&cs);
}

static void startPoke(uint64 sch, const std::vector<anyID>& targets) {
    if (targets.empty()) return;
    EnterCriticalSection(&cs);
    stopJob(pokeJob);
    pokeJob.sch = sch;
    pokeJob.targets = targets;
    pokeJob.intervalMs = g.pokeDelayMs;
    pokeJob.limiterMs = g.limiterSec * 1000;
    launchJob(pokeJob, pokeThread);
    LeaveCriticalSection(&cs);
}

static void stopMove() { EnterCriticalSection(&cs); stopJob(moveJob); LeaveCriticalSection(&cs); }
static void stopPoke() { EnterCriticalSection(&cs); stopJob(pokeJob); LeaveCriticalSection(&cs); }

/* Stop the timed jobs only (used by Chaos: Stop and by shutdown). */
static void stopAllJobs() {
    EnterCriticalSection(&cs);
    stopJob(moveJob);
    stopJob(pokeJob);
    LeaveCriticalSection(&cs);
}

/* Failsafe: stop EVERYTHING - timed jobs AND the reactive features.
 * Auto-poke-back is turned off for the session (re-enable it in Settings);
 * all talk-poke arming is cleared. */
static void hardStop() {
    stopAllJobs();
    EnterCriticalSection(&cs);
    talkTargets.clear();      /* disarm every talk-poke target */
    talkMoveTargets.clear();  /* disarm every talk-move target */
    g.autoPokeBack = 0;       /* stop reacting to incoming pokes */
    LeaveCriticalSection(&cs);
    ts3Functions.printMessageToCurrentTab(
        "[Fun Plugin] HARD STOP: all jobs stopped, talk-poke/talk-move disarmed, auto-poke-back turned off.");
}

static std::vector<anyID> one(anyID c) { return std::vector<anyID>(1, c); }

/* ------------------------------------------------------------------ */
/* Delayed talk reactions                                             */
/* ------------------------------------------------------------------ */

struct DelayedReaction {
    uint64 sch;
    anyID  client;
    int    delayMs;
    bool   doPoke;  int pokeN;  bool useMsg;  char msg[256];
    bool   doMove;  int moveAll;
};

static DWORD WINAPI reactionThread(LPVOID p) {
    DelayedReaction* d = (DelayedReaction*)p;
    /* wait out the delay, but bail immediately if the plugin is shutting down */
    if (WaitForSingleObject(g_shutdownEvent, d->delayMs) == WAIT_TIMEOUT) {
        if (d->doPoke) {
            if (d->useMsg) pokeTimesMsg(d->sch, d->client, d->pokeN, d->msg);
            else           pokeTimes(d->sch, d->client, d->pokeN);
        }
        if (d->doMove) {
            Job tmp{};
            tmp.sch = d->sch;
            tmp.targets.push_back(d->client);
            tmp.allChannels = d->moveAll;
            doOneMove(&tmp);
        }
    }
    free(d);
    InterlockedDecrement(&g_pending);
    return 0;
}

static void scheduleReaction(const DelayedReaction& src) {
    DelayedReaction* d = (DelayedReaction*)malloc(sizeof(DelayedReaction));
    *d = src;
    InterlockedIncrement(&g_pending);
    HANDLE h = CreateThread(NULL, 0, reactionThread, d, 0, NULL);
    if (h) CloseHandle(h);
    else { InterlockedDecrement(&g_pending); free(d); }
}

/* ------------------------------------------------------------------ */
/* Self-lock (anti-move for my own client)                            */
/* ------------------------------------------------------------------ */

static std::string channelPathOf(uint64 sch, uint64 ch);   /* defined below */

/* forcedByOther == true  -> someone moved/kicked me: snap back to the lock channel
 * forcedByOther == false -> I moved myself: re-anchor the lock to where I went  */
static void selfMovedHandler(uint64 sch, anyID clientID, uint64 newCh, bool forcedByOther) {
    LONG   locked;
    uint64 lockSch, lockCh;
    EnterCriticalSection(&cs);
    locked  = selfLock;
    lockSch = selfLockSch;
    lockCh  = selfLockChannel;
    LeaveCriticalSection(&cs);

    if (!locked || sch != lockSch) return;
    if (clientID != ownClient(sch)) return;

    if (forcedByOther) {
        if (newCh != lockCh)
            ts3Functions.requestClientMove(sch, clientID, lockCh, "", NULL);   /* snap back */
    } else {
        std::string path = channelPathOf(sch, newCh);        /* keep saved channel current */
        EnterCriticalSection(&cs);
        if (selfLock && sch == selfLockSch) {
            selfLockChannel = newCh;                          /* re-anchor */
            if (g_reconn.valid) {
                strncpy(g_reconn.channelPath, path.c_str(), sizeof(g_reconn.channelPath) - 1);
                g_reconn.channelPath[sizeof(g_reconn.channelPath) - 1] = '\0';
            }
        }
        LeaveCriticalSection(&cs);
    }
}

/* build a channel path like "Parent/Child/Leaf" (needed by guiConnect on rejoin) */
static std::string channelPathOf(uint64 sch, uint64 ch) {
    std::vector<std::string> parts;
    uint64 c = ch;
    for (int guard = 0; c != 0 && guard < 64; ++guard) {
        char* name = NULL;
        if (ts3Functions.getChannelVariableAsString(sch, c, CHANNEL_NAME, &name) == ERROR_ok && name) {
            parts.push_back(name);
            ts3Functions.freeMemory(name);
        }
        uint64 parent = 0;
        if (ts3Functions.getParentChannelOfChannel(sch, c, &parent) != ERROR_ok) break;
        c = parent;
    }
    std::string path;
    for (size_t i = parts.size(); i-- > 0; ) {
        if (!path.empty()) path += "/";
        path += parts[i];
    }
    return path;
}

/* snapshot everything needed to reconnect later (must be called while connected).
 * channelOverride != 0 forces the channel (e.g. the kick event's oldChannelID). */
static void captureReconnectInfo(uint64 sch, uint64 channelOverride) {
    char host[256]; host[0] = '\0';
    unsigned short port = 0;
    char pw[256]; pw[0] = '\0';
    ts3Functions.getServerConnectInfo(sch, host, &port, pw, sizeof(host));

    std::string nick;
    char* n = NULL;
    if (ts3Functions.getClientSelfVariableAsString(sch, CLIENT_NICKNAME, &n) == ERROR_ok && n) {
        nick = n;
        ts3Functions.freeMemory(n);
    }

    std::string sname;
    char* sv = NULL;
    if (ts3Functions.getServerVariableAsString(sch, VIRTUALSERVER_NAME, &sv) == ERROR_ok && sv) {
        sname = sv;
        ts3Functions.freeMemory(sv);
    }

    InterlockedExchange(&g_myId, (LONG)ownClient(sch));

    uint64 ch = channelOverride;
    if (ch == 0) ts3Functions.getChannelOfClient(sch, ownClient(sch), &ch);
    std::string path = channelPathOf(sch, ch);

    EnterCriticalSection(&cs);
    g_reconn.valid = (host[0] != '\0');
    _snprintf(g_reconn.address, sizeof(g_reconn.address), "%s:%u", host, (unsigned)port);
    g_reconn.address[sizeof(g_reconn.address) - 1] = '\0';
    strncpy(g_reconn.password, pw, sizeof(g_reconn.password) - 1);
    g_reconn.password[sizeof(g_reconn.password) - 1] = '\0';
    strncpy(g_reconn.nickname, nick.c_str(), sizeof(g_reconn.nickname) - 1);
    g_reconn.nickname[sizeof(g_reconn.nickname) - 1] = '\0';
    strncpy(g_reconn.serverName, sname.c_str(), sizeof(g_reconn.serverName) - 1);
    g_reconn.serverName[sizeof(g_reconn.serverName) - 1] = '\0';
    strncpy(g_reconn.channelPath, path.c_str(), sizeof(g_reconn.channelPath) - 1);
    g_reconn.channelPath[sizeof(g_reconn.channelPath) - 1] = '\0';
    LeaveCriticalSection(&cs);
}

static DWORD WINAPI rejoinThread(LPVOID p) {
    (void)p;
    /* brief settle time after the disconnect; bail if shutting down */
    if (WaitForSingleObject(g_shutdownEvent, 500) == WAIT_OBJECT_0) {
        InterlockedDecrement(&g_pending);
        return 0;
    }
    ReconnectInfo info;
    EnterCriticalSection(&cs);
    info = g_reconn;
    LeaveCriticalSection(&cs);

    if (info.valid) {
        uint64 newsch = 0;
        /* guiConnect takes the target channel directly, so the client joins the
         * right channel as part of connecting - no bookmark walk, no second thread. */
        ts3Functions.guiConnect(PLUGIN_CONNECT_TAB_CURRENT, "", info.address, info.password,
                                info.nickname, info.channelPath, "",
                                "", "", "", "", "", "", "", &newsch);
    }
    InterlockedDecrement(&g_pending);
    return 0;
}

static void scheduleRejoin() {
    InterlockedIncrement(&g_pending);
    HANDLE h = CreateThread(NULL, 0, rejoinThread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else InterlockedDecrement(&g_pending);
}

/* ------------------------------------------------------------------ */
/* Settings dialog                                                    */
/* ------------------------------------------------------------------ */

static void setDlgInt(HWND h, int id, int v) {
    char buf[32];
    _snprintf(buf, sizeof(buf), "%d", v);
    SetDlgItemTextA(h, id, buf);
}

static int getDlgInt(HWND h, int id, int fallback) {
    BOOL ok = FALSE;
    int v = (int)GetDlgItemInt(h, id, &ok, FALSE);
    return ok ? v : fallback;
}

/* show a millisecond value as decimal seconds (e.g. 500 -> "0.5") */
static void setDlgSec(HWND h, int id, int ms) {
    char buf[32];
    _snprintf(buf, sizeof(buf), "%g", ms / 1000.0);
    SetDlgItemTextA(h, id, buf);
}

/* read a decimal-seconds field and return milliseconds (floored at 10ms) */
static int getDlgMs(HWND h, int id, int fallbackMs) {
    char buf[32];
    buf[0] = '\0';
    GetDlgItemTextA(h, id, buf, sizeof(buf));
    double s = atof(buf);
    if (s <= 0) return fallbackMs;
    int ms = (int)(s * 1000.0 + 0.5);
    if (ms < 10) ms = 10;   /* hard floor; see the flood-protection note in the README */
    return ms;
}

/* like getDlgMs but allows 0 = instant (used for the talk reaction delay) */
static int getDlgDelayMs(HWND h, int id, int fallbackMs) {
    char buf[32];
    buf[0] = '\0';
    GetDlgItemTextA(h, id, buf, sizeof(buf));
    if (buf[0] == '\0') return fallbackMs;
    double s = atof(buf);
    if (s <= 0) return 0;
    return (int)(s * 1000.0 + 0.5);
}

static INT_PTR CALLBACK settingsProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        setDlgSec(hDlg, IDC_MOVE_INTERVAL, g.moveIntervalMs);
        CheckRadioButton(hDlg, IDC_NE_ASK, IDC_NE_SKIP,
                         g.noEmptyMode == NE_ALL ? IDC_NE_ALL :
                         g.noEmptyMode == NE_SKIP ? IDC_NE_SKIP : IDC_NE_ASK);
        CheckDlgButton(hDlg, IDC_AUTOCREATE, g.autoCreateTemp ? BST_CHECKED : BST_UNCHECKED);

        setDlgSec(hDlg, IDC_POKE_DELAY, g.pokeDelayMs);
        CheckRadioButton(hDlg, IDC_PM_CUSTOM, IDC_PM_RANDCHARS,
                         g.pokeMode == PM_RANDOM    ? IDC_PM_RANDOM :
                         g.pokeMode == PM_RANDCHARS ? IDC_PM_RANDCHARS : IDC_PM_CUSTOM);
        setDlgInt(hDlg, IDC_RANDLEN, g.randCharLen);
        SetDlgItemTextA(hDlg, IDC_CUSTOMMSG, g.customMsg);

        CheckDlgButton(hDlg, IDC_AUTOPOKEBACK, g.autoPokeBack ? BST_CHECKED : BST_UNCHECKED);
        setDlgInt(hDlg, IDC_APB_TIMES, g.autoPokeBackTimes);
        setDlgInt(hDlg, IDC_TALK_TIMES, g.talkPokeTimes);
        setDlgSec(hDlg, IDC_TALK_DELAY, g.talkDelayMs);
        SetDlgItemTextA(hDlg, IDC_TALK_MSG, g.talkPokeMsg);

        setDlgInt(hDlg, IDC_LIMITER, g.limiterSec);

        /* join random messages with CRLF for the multiline edit */
        std::string joined;
        for (size_t i = 0; i < g.randomMsgs.size(); ++i) {
            if (i) joined += "\r\n";
            joined += g.randomMsgs[i];
        }
        SetDlgItemTextA(hDlg, IDC_RANDOMMSGS, joined.c_str());
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK: {
            g.moveIntervalMs = getDlgMs(hDlg, IDC_MOVE_INTERVAL, g.moveIntervalMs);
            g.noEmptyMode = IsDlgButtonChecked(hDlg, IDC_NE_ALL) ? NE_ALL :
                            IsDlgButtonChecked(hDlg, IDC_NE_SKIP) ? NE_SKIP : NE_ASK;
            g.autoCreateTemp = IsDlgButtonChecked(hDlg, IDC_AUTOCREATE) ? 1 : 0;

            g.pokeDelayMs = getDlgMs(hDlg, IDC_POKE_DELAY, g.pokeDelayMs);
            g.pokeMode = IsDlgButtonChecked(hDlg, IDC_PM_RANDOM)    ? PM_RANDOM :
                         IsDlgButtonChecked(hDlg, IDC_PM_RANDCHARS) ? PM_RANDCHARS : PM_CUSTOM;
            g.randCharLen = getDlgInt(hDlg, IDC_RANDLEN, g.randCharLen);
            if (g.randCharLen < 1)   g.randCharLen = 1;
            if (g.randCharLen > 100) g.randCharLen = 100;
            GetDlgItemTextA(hDlg, IDC_CUSTOMMSG, g.customMsg, sizeof(g.customMsg));

            g.autoPokeBack = IsDlgButtonChecked(hDlg, IDC_AUTOPOKEBACK) ? 1 : 0;
            g.autoPokeBackTimes = getDlgInt(hDlg, IDC_APB_TIMES, g.autoPokeBackTimes);
            if (g.autoPokeBackTimes < 1)   g.autoPokeBackTimes = 1;
            if (g.autoPokeBackTimes > 100) g.autoPokeBackTimes = 100;
            g.talkPokeTimes = getDlgInt(hDlg, IDC_TALK_TIMES, g.talkPokeTimes);
            if (g.talkPokeTimes < 1)   g.talkPokeTimes = 1;
            if (g.talkPokeTimes > 100) g.talkPokeTimes = 100;
            g.talkDelayMs = getDlgDelayMs(hDlg, IDC_TALK_DELAY, g.talkDelayMs);
            GetDlgItemTextA(hDlg, IDC_TALK_MSG, g.talkPokeMsg, sizeof(g.talkPokeMsg));

            g.limiterSec = getDlgInt(hDlg, IDC_LIMITER, g.limiterSec);
            if (g.limiterSec < 0) g.limiterSec = 0;

            /* split the multiline edit back into a list */
            char buf[4096];
            GetDlgItemTextA(hDlg, IDC_RANDOMMSGS, buf, sizeof(buf));
            g.randomMsgs.clear();
            std::string line;
            for (char* p = buf; ; ++p) {
                if (*p == '\r') continue;
                if (*p == '\n' || *p == '\0') {
                    if (!line.empty()) g.randomMsgs.push_back(line);
                    line.clear();
                    if (*p == '\0') break;
                } else {
                    line += *p;
                }
            }
            saveSettings();
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void openSettings() {
    DialogBoxParamA(hInstance, MAKEINTRESOURCEA(IDD_SETTINGS),
                    GetForegroundWindow(), settingsProc, 0);
}

/* ------------------------------------------------------------------ */
/* Menu creation helper                                               */
/* ------------------------------------------------------------------ */

static struct PluginMenuItem* mkItem(enum PluginMenuType type, int id, const char* text) {
    struct PluginMenuItem* m = (struct PluginMenuItem*)malloc(sizeof(struct PluginMenuItem));
    m->type = type;
    m->id = id;
    strncpy(m->text, text, PLUGIN_MENU_BUFSZ - 1);
    m->text[PLUGIN_MENU_BUFSZ - 1] = '\0';
    m->icon[0] = '\0';
    return m;
}

/* ================================================================== */
/* Required plugin exports                                            */
/* ================================================================== */

const char* ts3plugin_name()        { return PLUGIN_NAME; }
const char* ts3plugin_version()     { return PLUGIN_VERSION; }
int         ts3plugin_apiVersion()  { return PLUGIN_API_VERSION; }
const char* ts3plugin_author()      { return PLUGIN_AUTHOR; }
const char* ts3plugin_description() { return PLUGIN_DESC; }

void ts3plugin_setFunctionPointers(const struct TS3Functions funcs) {
    ts3Functions = funcs;
}

int ts3plugin_init() {
    InitializeCriticalSection(&cs);
    g_shutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);   /* manual reset */
    srand((unsigned)time(NULL));
    loadSettings();
    return 0;   /* success */
}

void ts3plugin_shutdown() {
    InterlockedExchange(&selfLock, 0);

    /* cancel and drain any pending delayed reactions before we lose ts3Functions */
    if (g_shutdownEvent) SetEvent(g_shutdownEvent);
    for (int i = 0; i < 300 && g_pending > 0; ++i) Sleep(10);   /* up to ~3s */

    stopAllJobs();
    EnterCriticalSection(&cs);
    talkTargets.clear();
    talkMoveTargets.clear();
    pokeBackCooldown.clear();
    talkCooldown.clear();
    talkMoveCooldown.clear();
    LeaveCriticalSection(&cs);
    DeleteCriticalSection(&cs);

    if (g_shutdownEvent) {
        CloseHandle(g_shutdownEvent);
        g_shutdownEvent = NULL;
    }
    if (pluginID) {
        free(pluginID);
        pluginID = NULL;
    }
}

/* ================================================================== */
/* Optional plugin exports                                            */
/* ================================================================== */

int ts3plugin_offersConfigure() {
    /* We open our own native dialog from the menu instead. */
    return PLUGIN_OFFERS_NO_CONFIGURE;
}

void ts3plugin_registerPluginID(const char* id) {
    const size_t sz = strlen(id) + 1;
    pluginID = (char*)malloc(sz);
    memcpy(pluginID, id, sz);
}

void ts3plugin_freeMemory(void* data) {
    free(data);
}

void ts3plugin_initMenus(struct PluginMenuItem*** menuItems, char** menuIcon) {
    const int count = 17;
    *menuItems = (struct PluginMenuItem**)malloc(sizeof(struct PluginMenuItem*) * (count + 1));
    int n = 0;
    /* Right-click a USER */
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_CLIENT,  MENU_MOVE_START,     "Random Move: Start");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_CLIENT,  MENU_MOVE_STOP,      "Random Move: Stop");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_CLIENT,  MENU_POKE_START,     "Poke: Start");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_CLIENT,  MENU_POKE_STOP,      "Poke: Stop");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_CLIENT,  MENU_CHAOS_START,    "Chaos (move + poke): Start");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_CLIENT,  MENU_CHAOS_STOP,     "Chaos (move + poke): Stop");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_CLIENT,  MENU_TALKPOKE_START, "Talk-poke: Arm");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_CLIENT,  MENU_TALKPOKE_STOP,  "Talk-poke: Disarm");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_CLIENT,  MENU_TALKMOVE_START, "Talk-move: Arm");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_CLIENT,  MENU_TALKMOVE_STOP,  "Talk-move: Disarm");
    /* Right-click a CHANNEL -> everyone in it */
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_CHANNEL, MENU_MOVE_START_CH,  "Random Move: Start (everyone here)");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_CHANNEL, MENU_POKE_START_CH,  "Poke: Start (everyone here)");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_CHANNEL, MENU_CHAOS_START_CH, "Chaos (move + poke): Start (everyone here)");
    /* Global plugin menu */
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_GLOBAL,  MENU_SETTINGS,       "Settings...");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_GLOBAL,  MENU_HARDSTOP,       "HARD STOP - stop everything");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_GLOBAL,  MENU_SELFLOCK_ON,    "Self-lock: ON (can't be moved out)");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_GLOBAL,  MENU_SELFLOCK_OFF,   "Self-lock: OFF");
    (*menuItems)[n++] = NULL;

    *menuIcon = NULL;
}

void ts3plugin_onMenuItemEvent(uint64 serverConnectionHandlerID, enum PluginMenuType type,
                               int menuItemID, uint64 selectedItemID) {
    anyID  clientTarget  = (anyID)selectedItemID;   /* CLIENT menus  */
    uint64 channelTarget = selectedItemID;          /* CHANNEL menus */

    switch (menuItemID) {
    /* ---- single user ---- */
    case MENU_MOVE_START:
        if (type == PLUGIN_MENU_TYPE_CLIENT) startMove(serverConnectionHandlerID, one(clientTarget));
        break;
    case MENU_MOVE_STOP:
        stopMove();
        break;
    case MENU_POKE_START:
        if (type == PLUGIN_MENU_TYPE_CLIENT) startPoke(serverConnectionHandlerID, one(clientTarget));
        break;
    case MENU_POKE_STOP:
        stopPoke();
        break;
    case MENU_CHAOS_START:
        if (type == PLUGIN_MENU_TYPE_CLIENT) {
            startMove(serverConnectionHandlerID, one(clientTarget));
            startPoke(serverConnectionHandlerID, one(clientTarget));
        }
        break;
    case MENU_CHAOS_STOP:
        stopAllJobs();
        break;
    case MENU_TALKPOKE_START:
        if (type == PLUGIN_MENU_TYPE_CLIENT) {
            EnterCriticalSection(&cs);
            talkTargets.insert(clientTarget);
            LeaveCriticalSection(&cs);
            ts3Functions.printMessageToCurrentTab("[Fun Plugin] Talk-poke armed for the selected user.");
        }
        break;
    case MENU_TALKPOKE_STOP:
        if (type == PLUGIN_MENU_TYPE_CLIENT) {
            EnterCriticalSection(&cs);
            talkTargets.erase(clientTarget);
            LeaveCriticalSection(&cs);
            ts3Functions.printMessageToCurrentTab("[Fun Plugin] Talk-poke disarmed for the selected user.");
        }
        break;
    case MENU_TALKMOVE_START:
        if (type == PLUGIN_MENU_TYPE_CLIENT) {
            EnterCriticalSection(&cs);
            talkMoveTargets.insert(clientTarget);
            LeaveCriticalSection(&cs);
            ts3Functions.printMessageToCurrentTab("[Fun Plugin] Talk-move armed: this user gets moved whenever they talk.");
        }
        break;
    case MENU_TALKMOVE_STOP:
        if (type == PLUGIN_MENU_TYPE_CLIENT) {
            EnterCriticalSection(&cs);
            talkMoveTargets.erase(clientTarget);
            LeaveCriticalSection(&cs);
            ts3Functions.printMessageToCurrentTab("[Fun Plugin] Talk-move disarmed for the selected user.");
        }
        break;

    /* ---- whole channel (group) ---- */
    case MENU_MOVE_START_CH:
        if (type == PLUGIN_MENU_TYPE_CHANNEL)
            startMove(serverConnectionHandlerID, clientsInChannel(serverConnectionHandlerID, channelTarget));
        break;
    case MENU_POKE_START_CH:
        if (type == PLUGIN_MENU_TYPE_CHANNEL)
            startPoke(serverConnectionHandlerID, clientsInChannel(serverConnectionHandlerID, channelTarget));
        break;
    case MENU_CHAOS_START_CH:
        if (type == PLUGIN_MENU_TYPE_CHANNEL) {
            std::vector<anyID> v = clientsInChannel(serverConnectionHandlerID, channelTarget);
            startMove(serverConnectionHandlerID, v);
            startPoke(serverConnectionHandlerID, v);
        }
        break;

    /* ---- global ---- */
    case MENU_SETTINGS:
        openSettings();
        break;
    case MENU_HARDSTOP:
        hardStop();
        break;
    case MENU_SELFLOCK_ON: {
        anyID me = ownClient(serverConnectionHandlerID);
        uint64 ch = 0;
        ts3Functions.getChannelOfClient(serverConnectionHandlerID, me, &ch);
        EnterCriticalSection(&cs);
        selfLock = 1;
        selfLockSch = serverConnectionHandlerID;
        selfLockChannel = ch;
        LeaveCriticalSection(&cs);
        g.selfLockOn = 1;                                      /* persist + auto-arm next time */
        saveSettings();
        captureReconnectInfo(serverConnectionHandlerID, 0);   /* enable server-kick rejoin */
        ts3Functions.printMessageToCurrentTab(
            "[Fun Plugin] Self-lock ON (stays on across restarts): if anyone moves, channel-kicks, "
            "or SERVER-kicks you, you'll come right back. (You can still switch channels yourself.)");
        break;
    }
    case MENU_SELFLOCK_OFF:
        EnterCriticalSection(&cs);
        selfLock = 0;
        LeaveCriticalSection(&cs);
        g.selfLockOn = 0;
        saveSettings();
        ts3Functions.printMessageToCurrentTab("[Fun Plugin] Self-lock OFF.");
        break;
    }
}

/* ---- Reactions: automatic poke-back and talk-poke ---- */

int ts3plugin_onClientPokeEvent(uint64 serverConnectionHandlerID, anyID fromClientID,
                                const char* pokerName, const char* pokerUniqueIdentity,
                                const char* message, int ffIgnored) {
    (void)pokerName; (void)pokerUniqueIdentity; (void)message; (void)ffIgnored;

    if (g.autoPokeBack) {
        DWORD now = GetTickCount();
        bool ok;
        int times;
        EnterCriticalSection(&cs);
        DWORD last = pokeBackCooldown[fromClientID];
        ok = (last == 0) || ((now - last) > 5000);   /* 5s cooldown breaks ping-pong loops */
        if (ok) pokeBackCooldown[fromClientID] = now;
        times = g.autoPokeBackTimes;
        LeaveCriticalSection(&cs);

        if (ok) pokeTimes(serverConnectionHandlerID, fromClientID, times);
    }
    return 0;   /* 0 = let the incoming poke display normally */
}

void ts3plugin_onTalkStatusChangeEvent(uint64 serverConnectionHandlerID, int status,
                                       int isReceivedWhisper, anyID clientID) {
    (void)isReceivedWhisper;
    if (status != STATUS_TALKING) return;

    DelayedReaction d = {};
    d.sch = serverConnectionHandlerID;
    d.client = clientID;
    bool any = false;
    DWORD now = GetTickCount();

    EnterCriticalSection(&cs);
    d.delayMs = g.talkDelayMs;
    if (talkTargets.count(clientID)) {
        DWORD last = talkCooldown[clientID];
        if ((last == 0) || ((now - last) > 1000)) {   /* at most one trigger/sec per user */
            talkCooldown[clientID] = now;
            d.doPoke = true;
            d.pokeN = g.talkPokeTimes;
            d.useMsg = (g.talkPokeMsg[0] != 0);
            if (d.useMsg) {
                strncpy(d.msg, g.talkPokeMsg, sizeof(d.msg) - 1);
                d.msg[sizeof(d.msg) - 1] = '\0';
            }
            any = true;
        }
    }
    if (talkMoveTargets.count(clientID)) {
        DWORD last = talkMoveCooldown[clientID];
        if ((last == 0) || ((now - last) > 1000)) {
            talkMoveCooldown[clientID] = now;
            d.doMove = true;
            d.moveAll = (g.noEmptyMode == NE_ALL) ? 1 : 0;   /* same setting; never prompts here */
            any = true;
        }
    }
    LeaveCriticalSection(&cs);

    /* fire after the configured delay so the victim gets a word out first */
    if (any) scheduleReaction(d);
}

/* ---- Self-lock: react to my own client being moved/kicked ---- */

void ts3plugin_onClientMoveEvent(uint64 serverConnectionHandlerID, anyID clientID,
                                 uint64 oldChannelID, uint64 newChannelID,
                                 int visibility, const char* moveMessage) {
    (void)oldChannelID; (void)visibility; (void)moveMessage;
    /* a plain move event is a voluntary/self switch -> re-anchor */
    selfMovedHandler(serverConnectionHandlerID, clientID, newChannelID, false);
}

void ts3plugin_onClientMoveMovedEvent(uint64 serverConnectionHandlerID, anyID clientID,
                                      uint64 oldChannelID, uint64 newChannelID, int visibility,
                                      anyID moverID, const char* moverName,
                                      const char* moverUniqueIdentifier, const char* moveMessage) {
    (void)oldChannelID; (void)visibility; (void)moverName;
    (void)moverUniqueIdentifier; (void)moveMessage;
    selfMovedHandler(serverConnectionHandlerID, clientID, newChannelID,
                     moverID != ownClient(serverConnectionHandlerID));
}

void ts3plugin_onClientKickFromChannelEvent(uint64 serverConnectionHandlerID, anyID clientID,
                                            uint64 oldChannelID, uint64 newChannelID, int visibility,
                                            anyID kickerID, const char* kickerName,
                                            const char* kickerUniqueIdentifier, const char* kickMessage) {
    (void)oldChannelID; (void)visibility; (void)kickerName;
    (void)kickerUniqueIdentifier; (void)kickMessage;
    selfMovedHandler(serverConnectionHandlerID, clientID, newChannelID,
                     kickerID != ownClient(serverConnectionHandlerID));
}

/* A real server kick of my own client sets the kick flag (consumed on disconnect). */
void ts3plugin_onClientKickFromServerEvent(uint64 serverConnectionHandlerID, anyID clientID,
                                           uint64 oldChannelID, uint64 newChannelID, int visibility,
                                           anyID kickerID, const char* kickerName,
                                           const char* kickerUniqueIdentifier, const char* kickMessage) {
    (void)oldChannelID; (void)newChannelID; (void)visibility;
    (void)kickerName; (void)kickerUniqueIdentifier; (void)kickMessage; (void)kickerID;
    anyID meNow = ownClient(serverConnectionHandlerID);
    if (!selfLock) return;
    if (clientID == (anyID)g_myId || clientID == meNow) {
        /* channel is tracked continuously (connect + moves); don't recapture here
         * because the client is already tearing down and would clobber it. */
        InterlockedExchange(&g_kickFlag, 1);
    }
}

/* Connection state changes drive the server-kick recovery. A voluntary
 * disconnect and a kick BOTH report err=0x0000, so we rely on g_kickFlag
 * (set by onClientKickFromServerEvent) to tell them apart. */
void ts3plugin_onConnectStatusChangeEvent(uint64 serverConnectionHandlerID, int newStatus,
                                          unsigned int errorNumber) {
    if (newStatus == STATUS_CONNECTION_ESTABLISHED) {
        InterlockedExchange(&g_rejoinAttempts, 0);
        if (g.selfLockOn) {
            InterlockedExchange(&selfLock, 1);   /* auto-arm self-lock on every (re)connect */
            uint64 ch = 0;
            ts3Functions.getChannelOfClient(serverConnectionHandlerID,
                                            ownClient(serverConnectionHandlerID), &ch);
            EnterCriticalSection(&cs);
            selfLockSch = serverConnectionHandlerID;   /* re-anchor to the (re)connected server */
            selfLockChannel = ch;
            LeaveCriticalSection(&cs);
            captureReconnectInfo(serverConnectionHandlerID, 0);
        }
        return;
    }

    if (newStatus != STATUS_DISCONNECTED) return;

    bool kicked = (InterlockedExchange(&g_kickFlag, 0) == 1);
    if (!selfLock) return;

    (void)errorNumber;
    if (!kicked) return;   /* voluntary disconnect (or unknown) - leave it alone */

    bool infoValid;
    EnterCriticalSection(&cs);
    infoValid = g_reconn.valid;
    LeaveCriticalSection(&cs);
    if (!infoValid) return;

    if (InterlockedIncrement(&g_rejoinAttempts) > 5)   /* don't loop on a ban / dead server */
        return;
    scheduleRejoin();
}

/* ------------------------------------------------------------------ */
/* DllMain - capture the module handle for the dialog resource        */
/* ------------------------------------------------------------------ */

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        hInstance = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
