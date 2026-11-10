/* Offline timing/job harness: SDK calls are local counters, never network calls. */
#include <cassert>
#include <iostream>
#include <atomic>
#include "../src/plugin.cpp"

namespace {
std::atomic<int> requests{0}, scans{0};
int stopAt = 100, rejectAt = 0, replyAt = 0;
bool noChannels = false;
bool acknowledge = true;
uint64 currentChannel = 11, previousDestination = 0;
Job* activeJob = nullptr;
std::string config;
void configPath(char* buffer, size_t size) { snprintf(buffer, size, "%s", config.c_str()); }
void print(const char*) {}
unsigned int release(void* p) { free(p); return ERROR_ok; }
void returnCode(const char*, char* result, size_t size) {
    static int serial = 0; snprintf(result, size, "job-%d", ++serial);
}
unsigned int sent(uint64 sch, const char* code) {
    int n = ++requests;
    if (acknowledge) ts3plugin_onServerErrorEvent(sch, "", n == replyAt ? ERROR_client_is_flooding : ERROR_ok, code, "");
    if (n == stopAt) SetEvent(activeJob->stopEvent);
    return n == rejectAt ? ERROR_client_is_flooding : ERROR_ok;
}
unsigned int poke(uint64 sch, anyID, const char* message, const char* code) {
    assert(message && *message); return sent(sch, code);
}
unsigned int move(uint64 sch, anyID, uint64 channel, const char*, const char* code) {
    assert(channel != currentChannel && channel != previousDestination);
    previousDestination = channel;
    currentChannel = channel; // Simulate promptly received move notifications.
    return sent(sch, code);
}
unsigned int channels(uint64, uint64** result) {
    ++scans;
    *result = (uint64*)calloc(5, sizeof(uint64));
    if (!noChannels) for (int i = 0; i < 4; ++i) (*result)[i] = 11 + i;
    return ERROR_ok;
}
unsigned int members(uint64, uint64 channel, anyID** result) {
    *result = (anyID*)calloc(2, sizeof(anyID));
    if (channel == currentChannel) (*result)[0] = 42;
    return ERROR_ok;
}
unsigned int where(uint64, anyID, uint64* result) { *result = currentChannel; return ERROR_ok; }
void prepare(Job& job, int interval = 0) {
    requests = 0; scans = 0; stopAt = 100; rejectAt = replyAt = 0;
    noChannels = false; currentChannel = 11; previousDestination = 0;
    acknowledge = true;
    g_sendWindow.cancelAll();
    job.sch = 1; job.targets = {42}; job.intervalMs = interval; job.limiterMs = 0; job.allChannels = 1;
    activeJob = &job;
}
double run(Job& job, LPTHREAD_START_ROUTINE procedure) {
    LARGE_INTEGER frequency, start, end;
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
    launchJob(job, procedure);
    assert(job.thread && WaitForSingleObject(job.thread, 5000) == WAIT_OBJECT_0);
    QueryPerformanceCounter(&end);
    stopJob(job);
    assert(!job.thread && !job.stopEvent && !job.delayTimer && !job.active);
    return 1000.0 * (end.QuadPart - start.QuadPart) / frequency.QuadPart;
}
}
int main() {
    InitializeCriticalSection(&cs); setDefaults();
    TS3Functions api = {};
    api.freeMemory = release; api.requestClientPoke = poke; api.requestClientMove = move;
    api.getChannelList = channels; api.getChannelClientList = members; api.getChannelOfClient = where;
    api.createReturnCode = returnCode; api.printMessageToCurrentTab = print; api.getConfigPath = configPath;
    ts3Functions = api; ts3plugin_registerPluginID("timing-test");
    hInstance = GetModuleHandleW(NULL);
    HWND dialog = CreateDialogParamW(hInstance, MAKEINTRESOURCEW(IDD_SETTINGS), NULL, settingsProc, 0);
    assert(dialog);
    for (auto item : {std::make_pair("0", 0), {"0.001", 1}, {"0.0001", 1}, {"0.01", 10}, {"1.25", 1250}}) {
        SetDlgItemTextA(dialog, IDC_MOVE_INTERVAL, item.first);
        assert(getDlgMs(dialog, IDC_MOVE_INTERVAL, 5000) == item.second);
    }
    for (const char* invalid : {"", " ", "garbage", "0oops", "nan", "inf", "-1", "999999999999999999"}) {
        SetDlgItemTextA(dialog, IDC_MOVE_INTERVAL, invalid);
        assert(getDlgMs(dialog, IDC_MOVE_INTERVAL, 5000) == 5000);
    }
    DestroyWindow(dialog);
    Job pending{};
    pending.lastDestination[42] = 12; pending.lastMoveAt[42] = GetTickCount64();
    assert(pendingDestination(&pending, 42, 11) == 12);
    assert(pendingDestination(&pending, 42, 12) == 0); // Server caught up.
    pending.lastDestination[42] = 12; pending.lastMoveAt[42] = GetTickCount64() - 1001;
    assert(pendingDestination(&pending, 42, 11) == 0); // External moves cannot stall a run.
    char temporary[MAX_PATH]; GetTempPathA(sizeof(temporary), temporary);
    config = std::string(temporary) + "funplugin-timing-" + std::to_string(GetCurrentProcessId()) + "\\";
    assert(CreateDirectoryA(config.c_str(), NULL));
    g.moveIntervalMs = 0; g.pokeDelayMs = 1; saveSettings(); setDefaults(); loadSettings();
    assert(g.moveIntervalMs == 0 && g.pokeDelayMs == 1);
    assert(DeleteFileA((config + "funplugin.ini").c_str())); assert(RemoveDirectoryA(config.c_str()));

    prepare(pokeJob, 10); double paced = run(pokeJob, pokeThread); assert(requests == 100);
    prepare(pokeJob); double maximum = run(pokeJob, pokeThread); assert(requests == 100);
    assert(maximum < paced); // Relative local-loop check, not a server throughput claim.
    prepare(pokeJob, 1); double oneMs = run(pokeJob, pokeThread); assert(requests == 100);
    prepare(moveJob); double moves = run(moveJob, moveThread); assert(requests == 100);
    prepare(pokeJob); pokeJob.targets = std::vector<anyID>(100, 42); stopAt = 7;
    run(pokeJob, pokeThread); assert(requests == 7); // Stop is checked within a group.
    prepare(pokeJob); rejectAt = 3; run(pokeJob, pokeThread); assert(requests == 3);
    prepare(pokeJob); replyAt = 4; run(pokeJob, pokeThread); assert(requests == 4);
    prepare(moveJob); replyAt = 4; run(moveJob, moveThread); assert(requests == 4);
    prepare(moveJob); noChannels = true; moveJob.limiterMs = 60;
    run(moveJob, moveThread); assert(!requests && scans < 20);
    prepare(pokeJob); stopAt = INT_MAX; pokeJob.limiterMs = 30;
    run(pokeJob, pokeThread); assert(requests > 0);
    prepare(pokeJob, 60000); stopAt = INT_MAX; launchJob(pokeJob, pokeThread);
    while (!requests.load()) Sleep(1);
    hardStop(); assert(!pokeJob.thread && !pokeJob.active); // Long waits interrupt immediately.
    prepare(pokeJob); acknowledge = false; stopAt = INT_MAX;
    launchJob(pokeJob, pokeThread);
    while (requests.load() < (int)SendWindow::capacity) Sleep(1);
    Sleep(30); hardStop();
    assert(requests == (int)SendWindow::capacity && !pokeJob.thread);
    prepare(pokeJob); acknowledge = false; stopAt = INT_MAX; pokeJob.limiterMs = 60;
    double limited = run(pokeJob, pokeThread);
    assert(requests == (int)SendWindow::capacity && limited < 1000);
    prepare(pokeJob, 60000); stopAt = INT_MAX; pokeJob.limiterMs = 60;
    assert(run(pokeJob, pokeThread) < 1000 && requests == 1);

    g_shutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    requests = 0; acknowledge = true;
    DelayedReaction delayed = {};
    delayed.epoch = g_sendWindow.epoch(); delayed.sch = 1; delayed.client = 42;
    delayed.delayMs = 200; delayed.doPoke = true; delayed.pokeN = 10;
    scheduleReaction(delayed); hardStop(); Sleep(250);
    assert(!requests && !g_reactionsPending.load());
    scheduleReaction(delayed); assert(!g_reactionsPending.load()); // Stale callback cannot re-arm.
    g.autoPokeBack = 1; g.autoPokeBackTimes = 100; acknowledge = false;
    ts3plugin_onClientPokeEvent(1, 42, "", "", "", 0);
    while (requests.load() < (int)SendWindow::capacity) Sleep(1);
    hardStop(); assert(!g_reactionsPending.load() && !g.autoPokeBack);
    assert(requests == (int)SendWindow::capacity);
    CloseHandle(g_shutdownEvent); g_shutdownEvent = NULL;

    // Pool worker and Stop share cs. Repeated cancellation must not join under that lock.
    g.autoCreateTemp = 1; g_tempChannels = {11, 12, 13};
    for (int i = 0; i < 20; ++i) {
        prepare(moveJob); stopAt = INT_MAX; launchJob(moveJob, moveThread);
        while (!requests.load()) Sleep(1);
        stopJob(moveJob);
    }
    g_tempChannels.clear(); g.autoCreateTemp = 0;
    free(pluginID); pluginID = NULL; DeleteCriticalSection(&cs);
    std::cout << "PASS: 0/1ms parsing and persistence, exact requests, stop/limiter, errors, idle backoff, pool cancellation\n"
              << "Offline 100-request loop ms: 10ms=" << paced << ", 1ms=" << oneMs
              << ", maximum poke=" << maximum << ", maximum move=" << moves << "\n";
}
