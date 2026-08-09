/*
 * Fun Plugin for TeamSpeak 3 (client-side, 64-bit)
 *
 * Two features, both driven from the right-click menu on a user:
 *   1) Random Move - keep moving a user into a random EMPTY channel on an interval.
 *   2) Poke        - keep poking a user with a custom or random message on a delay.
 *
 * Plus a global "HARD STOP" that halts everything and a native Settings dialog.
 *
 * Written for your own server where you are the owner (permissions are not an issue).
 */

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

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
#define PLUGIN_VERSION "1.0"
#define PLUGIN_AUTHOR  "kingtnt"
#define PLUGIN_DESC    "Random-move and poke your friends for fun."

/* Menu item ids */
enum {
    MENU_MOVE_START = 1,
    MENU_MOVE_STOP,
    MENU_POKE_START,
    MENU_POKE_STOP,
    MENU_CHAOS_START,
    MENU_CHAOS_STOP,
    MENU_SETTINGS,
    MENU_HARDSTOP
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
    char customMsg[256];
    std::vector<std::string> randomMsgs;
};

static Settings g;
static CRITICAL_SECTION cs;   /* guards job start/stop + settings access */

static void setDefaults() {
    g.moveIntervalMs  = 5000;
    g.noEmptyMode     = NE_ASK;
    g.autoCreateTemp  = 0;
    g.pokeDelayMs     = 3000;
    g.pokeMode        = PM_CUSTOM;
    g.randCharLen     = 8;
    g.limiterSec      = 0;
    strcpy(g.customMsg, "stop ignoring me!");
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

    g.moveIntervalMs  = GetPrivateProfileIntA("move", "intervalMs", g.moveIntervalMs, ini);
    g.noEmptyMode     = GetPrivateProfileIntA("move", "noEmptyMode", g.noEmptyMode, ini);
    g.autoCreateTemp  = GetPrivateProfileIntA("move", "autoCreate",  g.autoCreateTemp, ini);
    g.pokeDelayMs     = GetPrivateProfileIntA("poke", "delayMs",    g.pokeDelayMs, ini);
    g.pokeMode        = GetPrivateProfileIntA("poke", "mode",        g.pokeMode, ini);
    g.randCharLen     = GetPrivateProfileIntA("poke", "randLen",    g.randCharLen, ini);
    g.limiterSec      = GetPrivateProfileIntA("limiter", "seconds", g.limiterSec, ini);
    GetPrivateProfileStringA("poke", "custom", g.customMsg, g.customMsg, sizeof(g.customMsg), ini);

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

    /* clear any stale keys, then rewrite the list */
    WritePrivateProfileStringA("poke_random", NULL, NULL, ini);
    _snprintf(num, sizeof(num), "%d", (int)g.randomMsgs.size());
    WritePrivateProfileStringA("poke", "randomCount", num, ini);
    for (size_t i = 0; i < g.randomMsgs.size(); ++i) {
        char key[32];
        _snprintf(key, sizeof(key), "random%d", (int)i);
        WritePrivateProfileStringA("poke", key, g.randomMsgs[i].c_str(), ini);
    }
}

/* ------------------------------------------------------------------ */
/* Job engine                                                         */
/* ------------------------------------------------------------------ */

struct Job {
    volatile LONG active;
    HANDLE  thread;
    HANDLE  stopEvent;   /* manual-reset; signalled to stop            */
    uint64  sch;         /* server connection handler id               */
    anyID   target;      /* the victim client id                       */
    int     allChannels; /* move: 1 = allowed to use non-empty channels */
    int     intervalMs;  /* snapshot of the interval for this run       */
    int     limiterMs;   /* snapshot of auto-stop time; 0 = no limit    */
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

static void startJob(Job& j, LPTHREAD_START_ROUTINE fn) {
    stopJob(j);   /* replace any existing run cleanly */
    j.stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    InterlockedExchange(&j.active, 1);
    j.thread = CreateThread(NULL, 0, fn, &j, 0, NULL);
}

/* interruptible sleep; returns true if we were told to stop */
static bool sleepOrStop(Job* j, int ms) {
    if (ms < 50) ms = 50;   /* backstop; the real floor is applied on input */
    return WaitForSingleObject(j->stopEvent, ms) == WAIT_OBJECT_0;
}

/* true once the run has hit its auto-stop limit */
static bool limitReached(Job* j, DWORD startTick) {
    return j->limiterMs > 0 && (DWORD)(GetTickCount() - startTick) >= (DWORD)j->limiterMs;
}

/* ------------------------------------------------------------------ */
/* Channel helpers                                                    */
/* ------------------------------------------------------------------ */

/* returns true if at least one channel (other than the target's own) is empty */
static bool hasEmptyChannel(uint64 sch, anyID target) {
    uint64 cur = 0;
    ts3Functions.getChannelOfClient(sch, target, &cur);
    uint64* channels = NULL;
    if (ts3Functions.getChannelList(sch, &channels) != ERROR_ok || !channels) return false;

    bool found = false;
    for (int i = 0; channels[i] && !found; ++i) {
        if (channels[i] == cur) continue;
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
    /* keep the empty temp channel alive long enough to move someone into it */
    ts3Functions.setChannelVariableAsInt(sch, 0, CHANNEL_DELETE_DELAY, 60);
    ts3Functions.flushChannelCreation(sch, 0, NULL);   /* parent 0 = root */
}

static void doOneMove(Job* j) {
    uint64 cur = 0;
    ts3Functions.getChannelOfClient(j->sch, j->target, &cur);

    uint64* channels = NULL;
    if (ts3Functions.getChannelList(j->sch, &channels) != ERROR_ok || !channels) return;

    std::vector<uint64> empties;
    std::vector<uint64> all;
    for (int i = 0; channels[i]; ++i) {
        uint64 ch = channels[i];
        if (ch == cur) continue;             /* never "move" to where they already are */
        all.push_back(ch);
        anyID* cl = NULL;
        if (ts3Functions.getChannelClientList(j->sch, ch, &cl) == ERROR_ok && cl) {
            if (cl[0] == 0) empties.push_back(ch);
            ts3Functions.freeMemory(cl);
        }
    }
    ts3Functions.freeMemory(channels);

    uint64 dest = 0;
    if (!empties.empty()) {
        dest = empties[rand() % empties.size()];
    } else if (j->allChannels && !all.empty()) {
        dest = all[rand() % all.size()];
    } else if (g.autoCreateTemp) {
        createTempChannel(j->sch);           /* used automatically next cycle */
        return;
    } else {
        return;                              /* skip this cycle */
    }

    ts3Functions.requestClientMove(j->sch, j->target, dest, "", NULL);
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

static std::string randomChars(int n) {
    static const char cs[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    if (n < 1)   n = 1;
    if (n > 100) n = 100;
    std::string s;
    s.reserve(n);
    for (int i = 0; i < n; ++i) s += cs[rand() % (sizeof(cs) - 1)];
    return s;
}

static void doOnePoke(Job* j) {
    const char* msg = g.customMsg;
    std::string pick;
    if (g.pokeMode == PM_RANDOM && !g.randomMsgs.empty()) {
        pick = g.randomMsgs[rand() % g.randomMsgs.size()];
        msg = pick.c_str();
    } else if (g.pokeMode == PM_RANDCHARS) {
        pick = randomChars(g.randCharLen);
        msg = pick.c_str();
    }
    ts3Functions.requestClientPoke(j->sch, j->target, msg, NULL);
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

/* read a decimal-seconds field and return milliseconds (floored at 100ms) */
static int getDlgMs(HWND h, int id, int fallbackMs) {
    char buf[32];
    buf[0] = '\0';
    GetDlgItemTextA(h, id, buf, sizeof(buf));
    double s = atof(buf);
    if (s <= 0) return fallbackMs;
    int ms = (int)(s * 1000.0 + 0.5);
    if (ms < 100) ms = 100;   /* don't hammer the server / trip flood protection */
    return ms;
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
        setDlgInt(hDlg, IDC_LIMITER, g.limiterSec);
        SetDlgItemTextA(hDlg, IDC_CUSTOMMSG, g.customMsg);

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
            g.limiterSec = getDlgInt(hDlg, IDC_LIMITER, g.limiterSec);
            if (g.limiterSec < 0) g.limiterSec = 0;
            GetDlgItemTextA(hDlg, IDC_CUSTOMMSG, g.customMsg, sizeof(g.customMsg));

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
/* Menu action handlers                                               */
/* ------------------------------------------------------------------ */

static void onMoveStart(uint64 sch, anyID target) {
    /* decide the "no empty channel" behaviour up front, on the UI thread */
    int allMode = (g.noEmptyMode == NE_ALL) ? 1 : 0;
    if (g.noEmptyMode == NE_ASK && !hasEmptyChannel(sch, target)) {
        int r = MessageBoxA(GetForegroundWindow(),
                            "No empty channels are available right now.\n\n"
                            "Move the user among ALL channels anyway?",
                            "Random Move", MB_YESNO | MB_ICONWARNING);
        if (r == IDYES) allMode = 1;
        else return;   /* cancel start */
    }

    EnterCriticalSection(&cs);
    moveJob.sch = sch;
    moveJob.target = target;
    moveJob.allChannels = allMode;
    moveJob.intervalMs = g.moveIntervalMs;
    moveJob.limiterMs = g.limiterSec * 1000;
    startJob(moveJob, moveThread);
    LeaveCriticalSection(&cs);
}

static void onPokeStart(uint64 sch, anyID target) {
    EnterCriticalSection(&cs);
    pokeJob.sch = sch;
    pokeJob.target = target;
    pokeJob.intervalMs = g.pokeDelayMs;
    pokeJob.limiterMs = g.limiterSec * 1000;
    startJob(pokeJob, pokeThread);
    LeaveCriticalSection(&cs);
}

/* Chaos = move AND poke the same target at once. */
static void onChaosStart(uint64 sch, anyID target) {
    onMoveStart(sch, target);   /* may prompt if no empty channel + Ask mode */
    onPokeStart(sch, target);
}

static void onChaosStop() {
    EnterCriticalSection(&cs);
    stopJob(moveJob);
    stopJob(pokeJob);
    LeaveCriticalSection(&cs);
}

static void hardStop() {
    EnterCriticalSection(&cs);
    stopJob(moveJob);
    stopJob(pokeJob);
    LeaveCriticalSection(&cs);
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
    memset(&moveJob, 0, sizeof(moveJob));
    memset(&pokeJob, 0, sizeof(pokeJob));
    loadSettings();
    return 0;   /* success */
}

void ts3plugin_shutdown() {
    hardStop();
    DeleteCriticalSection(&cs);
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
    /* number of items + 1 for the terminating NULL */
    const int count = 8;
    *menuItems = (struct PluginMenuItem**)malloc(sizeof(struct PluginMenuItem*) * (count + 1));
    int n = 0;
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_CLIENT, MENU_MOVE_START,  "Random Move: Start");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_CLIENT, MENU_MOVE_STOP,   "Random Move: Stop");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_CLIENT, MENU_POKE_START,  "Poke: Start");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_CLIENT, MENU_POKE_STOP,   "Poke: Stop");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_CLIENT, MENU_CHAOS_START, "Chaos (move + poke): Start");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_CLIENT, MENU_CHAOS_STOP,  "Chaos (move + poke): Stop");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_GLOBAL, MENU_SETTINGS,    "Settings...");
    (*menuItems)[n++] = mkItem(PLUGIN_MENU_TYPE_GLOBAL, MENU_HARDSTOP,    "HARD STOP - stop everything");
    (*menuItems)[n++] = NULL;

    *menuIcon = NULL;
}

void ts3plugin_onMenuItemEvent(uint64 serverConnectionHandlerID, enum PluginMenuType type,
                               int menuItemID, uint64 selectedItemID) {
    anyID target = (anyID)selectedItemID;   /* for CLIENT menus this is the client id */

    switch (menuItemID) {
    case MENU_MOVE_START:
        if (type == PLUGIN_MENU_TYPE_CLIENT) onMoveStart(serverConnectionHandlerID, target);
        break;
    case MENU_MOVE_STOP:
        EnterCriticalSection(&cs);
        stopJob(moveJob);
        LeaveCriticalSection(&cs);
        break;
    case MENU_POKE_START:
        if (type == PLUGIN_MENU_TYPE_CLIENT) onPokeStart(serverConnectionHandlerID, target);
        break;
    case MENU_POKE_STOP:
        EnterCriticalSection(&cs);
        stopJob(pokeJob);
        LeaveCriticalSection(&cs);
        break;
    case MENU_CHAOS_START:
        if (type == PLUGIN_MENU_TYPE_CLIENT) onChaosStart(serverConnectionHandlerID, target);
        break;
    case MENU_CHAOS_STOP:
        onChaosStop();
        break;
    case MENU_SETTINGS:
        openSettings();
        break;
    case MENU_HARDSTOP:
        hardStop();
        break;
    }
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
