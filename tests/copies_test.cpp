/* SDK integration harness: no TeamSpeak server, network, or real config. */
#include <cassert>
#include <iostream>
#include "../src/plugin.cpp"

namespace {
struct FakeConnection {
    int status = STATUS_CONNECTION_ESTABLISHED;
    uint64 channel = 9;
    bool microphone = false, speaker = false, capture = false, playback = false;
    bool noAudioProfile = false;
};
std::map<uint64, FakeConnection> connections;
std::vector<uint64> stops;
std::vector<uint64> closed;
std::set<uint64> tabs;
uint64 focused = 1, savedFocus = 1;
bool allowClose = true;
std::vector<std::string> names, messages;
uint64 nextHandler = 100;
unsigned int connectError = ERROR_ok, muteError = ERROR_ok;
bool returnExisting = false;
std::string configPath;
std::string lastAddress, lastChannel, lastPassword;
int activations = 0, deviceCloses = 0;
Copies::Manager* duringOpen = nullptr;
bool stopDuringOpen = false;
bool delayDisconnect = false;

unsigned int freeFake(void* value) { free(value); return ERROR_ok; }
void printFake(uint64, const char* message, PluginMessageTarget) { messages.emplace_back(message); }
void printCurrentFake(const char* message) { messages.emplace_back(message); }
unsigned int errorFake(unsigned int, char** result) { *result = _strdup("mock server rejection"); return ERROR_ok; }
unsigned int statusFake(uint64 sch, int* result) {
    auto it = connections.find(sch);
    if (it == connections.end()) return ERROR_server_invalid_id;
    *result = it->second.status;
    return ERROR_ok;
}
unsigned int handlersFake(uint64** result) {
    *result = (uint64*)calloc(connections.size() + 1, sizeof(uint64));
    size_t i = 0;
    for (const auto& item : connections) (*result)[i++] = item.first;
    return ERROR_ok;
}
unsigned int getIntFake(uint64 sch, size_t flag, int* result) {
    assert(flag == CLIENT_INPUT_HARDWARE || flag == CLIENT_OUTPUT_HARDWARE);
    if (flag == CLIENT_INPUT_HARDWARE)
        *result = connections.at(sch).capture ? HARDWAREINPUT_ENABLED : HARDWAREINPUT_DISABLED;
    else *result = connections.at(sch).playback ? HARDWAREOUTPUT_ENABLED : HARDWAREOUTPUT_DISABLED;
    return ERROR_ok;
}
unsigned int setIntFake(uint64 sch, size_t flag, int value) {
    if (muteError) return muteError;
    if (flag == CLIENT_INPUT_MUTED) connections.at(sch).microphone = value != 0;
    else if (flag == CLIENT_OUTPUT_MUTED) connections.at(sch).speaker = value != 0;
    else assert(false);
    return ERROR_ok;
}
unsigned int flushFake(uint64, const char*) { return ERROR_ok_no_update; }
unsigned int closeCaptureFake(uint64 sch) { ++deviceCloses; connections.at(sch).capture = false; return ERROR_ok; }
unsigned int closePlaybackFake(uint64 sch) { ++deviceCloses; connections.at(sch).playback = false; return ERROR_ok; }
unsigned int activateFake(uint64 sch) {
    ++activations;
    for (auto& item : connections) item.second.capture = false;
    connections.at(sch).capture = true;
    return ERROR_ok;
}
unsigned int clientFake(uint64 sch, anyID* result) { *result = (anyID)sch; return ERROR_ok; }
unsigned int channelFake(uint64 sch, anyID, uint64* result) { *result = connections.at(sch).channel; return ERROR_ok; }
unsigned int stopFake(uint64 sch, const char*) {
    assert(sch >= 100);  // Original and unrelated tabs must never be disconnected.
    stops.push_back(sch);
    if (!delayDisconnect) connections.at(sch).status = STATUS_DISCONNECTED;
    return ERROR_ok;
}
unsigned int connectFake(PluginConnectTab tab, const char*, const char* address, const char*,
                          const char* name, const char* channel, const char* password,
                          const char* captureProfile, const char* playbackProfile, const char*, const char*, const char*,
                          const char*, const char*, uint64* result) {
    assert(tab == PLUGIN_CONNECT_TAB_NEW);
    assert(std::string(captureProfile) == "__NONE__" && std::string(playbackProfile) == "__NONE__");
    for (const auto& entry : connections)
        assert(entry.second.status != STATUS_CONNECTING); // Never overlap handshakes.
    names.emplace_back(name);
    lastAddress = address; lastChannel = channel; lastPassword = password;
    if (returnExisting) { *result = 1; return ERROR_ok; }
    if (connectError) { *result = 0; return connectError; }
    *result = nextHandler++;
    tabs.insert(*result); focused = *result;
    connections[*result].status = STATUS_CONNECTING;
    // None profiles prevent opening the default hardware or stealing capture.
    connections[*result].noAudioProfile = true;
    // Exercise callbacks arriving synchronously before guiConnect returns the ID.
    if (g_copyWindow) ts3plugin_onConnectStatusChangeEvent(*result, STATUS_CONNECTING, ERROR_ok);
    if (duringOpen) {
        duringOpen->tick(999999); // Simulate a nested GUI timer during tab creation.
        if (stopDuringOpen) duringOpen->stopAll();
    }
    return ERROR_ok;
}
void configFake(char* result, size_t length) { snprintf(result, length, "%s", configPath.c_str()); }

void reset() {
    connections.clear(); stops.clear(); names.clear(); messages.clear();
    connections[1].capture = connections[1].playback = true;
    connections[2].capture = false;
    tabs = {1, 2}; focused = savedFocus = 1; closed.clear(); allowClose = true;
    nextHandler = 100; connectError = muteError = ERROR_ok; returnExisting = false;
    activations = deviceCloses = 0; duringOpen = nullptr; stopDuringOpen = false; delayDisconnect = false;
}
bool beforeFake() { savedFocus = focused; return true; }
bool afterFake(uint64) { focused = savedFocus; return true; }
bool closeTabFake(uint64 sch) {
    assert(sch >= 100 && sch != focused);
    if (!allowClose) return false;
    tabs.erase(sch); closed.push_back(sch);
    return true;
}
Copies::Hooks testHooks() {
    Copies::Hooks hooks;
    hooks.beforeOpen = beforeFake; hooks.afterOpen = afterFake; hooks.closeTab = closeTabFake;
    return hooks;
}
Copies::Request request(int count = 5) {
    Copies::Request result;
    result.source = 1; result.channel = 9; result.count = count;
    result.address = "[::1]:9987";
    result.channelPath = "Parent/Party";
    result.channelPassword = "test-channel-password";
    result.serverPassword = "test-server-password";
    result.baseNickname = "Player";
    return result;
}
void establish(Copies::Manager& manager, uint64 sch, uint64 now) {
    connections.at(sch).status = STATUS_CONNECTION_ESTABLISHED;
    assert(connections.at(sch).noAudioProfile);
    manager.status(sch, STATUS_CONNECTION_ESTABLISHED, ERROR_ok, now);
}
void drain(Copies::Manager& manager, uint64 now = 1000000) {
    for (int i = 0; i < 60 && manager.count(); ++i) manager.tick(now + i * 50);
}
void pump() {
    MSG message;
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
}

int main() {
    TS3Functions api = {};
    api.freeMemory = freeFake; api.printMessage = printFake; api.printMessageToCurrentTab = printCurrentFake;
    api.getErrorMessage = errorFake; api.getConnectionStatus = statusFake;
    api.getServerConnectionHandlerList = handlersFake; api.getClientSelfVariableAsInt = getIntFake;
    api.setClientSelfVariableAsInt = setIntFake; api.flushClientSelfUpdates = flushFake;
    api.closeCaptureDevice = closeCaptureFake; api.closePlaybackDevice = closePlaybackFake;
    api.activateCaptureDevice = activateFake; api.getClientID = clientFake;
    api.getChannelOfClient = channelFake; api.stopConnection = stopFake;
    api.guiConnect = connectFake; api.getConfigPath = configFake;
    ts3plugin_setFunctionPointers(api);

    int count = 0;
    assert(Copies::parseCount("5", count) && count == 5);
    assert(Copies::parseCount("2147483647", count) && count == INT_MAX);
    for (const char* bad : {"", "0", "-1", "1.5", " 5", "5x", "2147483648", "99999999999999999999"})
        assert(!Copies::parseCount(bad, count));
    assert(Copies::nickname("Player", 3) == "Player #3");
    assert(Copies::nickname(std::string(50, 'x'), 123).size() == 30);
    const std::string unicode = "\xd8\xb3";
    std::string longName;
    for (int i = 0; i < 35; ++i) longName += unicode;
    std::string trimmed = Copies::nickname(longName, 1);
    assert(trimmed == longName.substr(0, 54) + " #1");

    reset();
    {
        Copies::Manager manager(api, testHooks());
        assert(manager.start(request(), 0));
        assert(!manager.start(request(), 0));
        manager.tick(0);
        assert(names.size() == 1 && focused == 1);
        manager.tick(50);
        assert(names.size() == 1 && focused == 1);
        for (int i = 0; i < 5; ++i) {
            manager.tick(i * 600);
            assert(names.size() == (size_t)i + 1);
            establish(manager, 100 + i, i * 600 + 100);
            assert(connections.at(100 + i).microphone && connections.at(100 + i).speaker);
            assert(!connections.at(100 + i).capture && !connections.at(100 + i).playback);
            assert(connections.at(1).capture);
            manager.tick(i * 600 + 599);
            assert(names.size() == (size_t)i + 1); // Wait through the settling period.
        }
        assert(!manager.busy() && manager.count() == 5);
        assert(lastAddress == "[::1]:9987" && lastChannel == "Parent/Party" && lastPassword == "test-channel-password");
        manager.stopAll(); assert(closed.size() == 2); // Bounded cleanup per GUI tick.
        drain(manager);
        assert(stops.size() == 5 && names.size() == 5);
        assert(closed.size() == 5 && tabs.size() == 2 && focused == 1);
        for (uint64 sch : stops) manager.status(sch, STATUS_DISCONNECTED, ERROR_ok, 0);
        assert(manager.count() == 0);
    }
    reset();
    {
        Copies::Manager manager(api, testHooks());
        assert(manager.start(request(INT_MAX), 0));  // Count is not capped at five or preallocated.
        manager.tick(0); manager.stopAll(); manager.tick(100000);
        assert(names.size() == 1 && stops.size() == 1 && closed.size() == 1 && !manager.busy());
    }
    reset();
    {
        Copies::Manager manager(api, testHooks());
        manager.start(request(), 0); manager.tick(0); establish(manager, 100, 100);
        manager.tick(1000);
        manager.status(101, STATUS_DISCONNECTED, ERROR_server_maxclients_reached, 1100);
        manager.tick(50000);
        drain(manager);
        assert(!manager.busy() && manager.count() == 1 && names.size() == 2);
        assert(messages.back().find("1/5") != std::string::npos);
        assert(messages.back().find("mock server rejection") != std::string::npos);
        manager.shutdown();
    }
    reset();
    {
        Copies::Manager manager(api, testHooks());
        connectError = ERROR_client_is_flooding;
        manager.start(request(), 0); manager.tick(0); manager.tick(50000);
        assert(!manager.busy() && names.size() == 1 && manager.count() == 0);
    }
    reset();
    {
        Copies::Manager manager(api, testHooks());
        manager.start(request(), 0); manager.tick(0); manager.tick(20000);
        assert(!manager.busy() && stops.size() == 1);
        establish(manager, 100, 21000);  // A late callback must not restart the queue.
        manager.tick(22000); assert(names.size() == 1);
    }
    reset();
    {
        Copies::Manager manager(api, testHooks());
        returnExisting = true;
        manager.start(request(), 0); manager.tick(0);
        assert(!manager.busy() && manager.count() == 0 && stops.empty());
        assert(connections.at(1).capture && !connections.at(1).microphone);
    }
    reset();
    {
        Copies::Manager manager(api, testHooks());
        manager.start(request(), 0); manager.tick(0);
        connections.at(100).channel = 99; establish(manager, 100, 100);
        assert(!manager.busy() && stops.size() == 1);  // No silent fallback to the wrong channel.
    }
    reset();
    {
        Copies::Manager manager(api, testHooks());
        manager.start(request(), 0); manager.tick(0);
        muteError = ERROR_parameter_invalid; establish(manager, 100, 100);
        assert(!manager.busy() && stops.size() == 1);
    }
    reset();
    {
        Copies::Manager manager(api, testHooks());
        manager.start(request(), 0); manager.tick(0);
        connections.at(1).status = STATUS_DISCONNECTED; manager.tick(1000);
        assert(!manager.busy() && stops.size() == 1);
    }

    // Regression for the captured real-client guiConnect/audio initialization
    // stall: even a large batch never opens/closes physical audio devices, or
    // repeatedly reactivates the original capture device.
    reset();
    {
        Copies::Manager manager(api, testHooks());
        manager.start(request(50), 0);
        for (uint64 now = 0; manager.busy() && now < 60000; now += 50) {
            manager.tick(now);
            int pending = 0;
            for (auto it = connections.rbegin(); it != connections.rend(); ++it) {
                if (it->second.status == STATUS_CONNECTING) ++pending;
            }
            assert(pending <= 1);
            for (auto it = connections.rbegin(); it != connections.rend(); ++it)
                if (it->second.status == STATUS_CONNECTING) establish(manager, it->first, now);
            assert(connections.at(1).capture && connections.at(1).playback && focused == 1);
        }
        assert(!manager.busy() && manager.count() == 50 && names.size() == 50);
        manager.stopAll();
        drain(manager);
        assert(manager.count() == 0 && closed.size() == 50 && tabs.size() == 2);
        assert(activations == 0 && deviceCloses == 0);
    }
    reset();
    {
        Copies::Manager manager(api, testHooks());
        manager.start(request(), 0); manager.tick(0);
        connections.at(100).playback = true; // A client ignoring None must fail closed.
        establish(manager, 100, 100);
        assert(!manager.busy() && stops.size() == 1);
        assert(messages.back().find("did not disable audio") != std::string::npos);
        manager.tick(1000);
        assert(connections.at(1).capture && activations == 0 && deviceCloses == 0);
    }
    reset();
    {
        Copies::Manager manager(api, testHooks());
        duringOpen = &manager;
        manager.start(request(), 0); manager.tick(0);
        assert(names.size() == 1); // Nested timers cannot recursively start more tabs.
        manager.stopAll();
    }
    reset();
    {
        Copies::Manager manager(api, testHooks());
        duringOpen = &manager; stopDuringOpen = true;
        manager.start(request(), 0); manager.tick(0); manager.tick(1000);
        assert(!manager.busy() && names.size() == 1 && stops.size() == 1 && tabs.size() == 2);
    }

    // Actual plugin lifecycle, menu wiring, queued callbacks and HARD STOP.
    reset();
    {
        Copies::Manager manager(api, testHooks());
        manager.start(request(100000), 0);
        manager.tick(0); manager.tick(50); manager.tick(100);
        assert(names.size() == 1);
        establish(manager, 100, 110);
        manager.tick(609); assert(names.size() == 1);
        manager.status(100, STATUS_CONNECTION_ESTABLISHED, ERROR_ok, 600); // Late callback extends settling.
        manager.tick(1099); assert(names.size() == 1);
        manager.tick(1100); assert(names.size() == 2);
        manager.stopAll();
        drain(manager);
        assert(closed.size() == 2 && tabs.size() == 2);
    }
    reset();
    {
        Copies::Manager manager(api, testHooks());
        manager.start(request(2), 0); manager.tick(0); establish(manager, 100, 100); manager.tick(600);
        allowClose = false;
        manager.stopAll(); drain(manager);
        assert(manager.count() == 2 && closed.empty());
        allowClose = true;
        manager.stopAll(); // Failed UI cleanup remains tracked and can be retried.
        assert(manager.count() == 0 && closed.size() == 2 && tabs.size() == 2);
    }
    reset();
    {
        Copies::Manager manager(api, testHooks());
        manager.start(request(2), 0); manager.tick(0); establish(manager, 100, 100); manager.tick(600);
        delayDisconnect = true;
        manager.stopAll(); manager.tick(100);
        assert(closed.empty() && manager.count() == 2); // Wait for real disconnect.
        connections.at(101).status = STATUS_DISCONNECTED;
        manager.status(101, STATUS_DISCONNECTED, ERROR_ok, 200); manager.tick(200);
        assert(closed.size() == 1 && closed[0] == 101 && manager.count() == 1);
        connections.at(100).status = STATUS_DISCONNECTED;
        manager.status(100, STATUS_DISCONNECTED, ERROR_ok, 300); manager.tick(300);
        assert(closed.size() == 2 && manager.count() == 0 && tabs.size() == 2);
    }
    reset();
    char temporary[MAX_PATH]; GetTempPathA(sizeof(temporary), temporary);
    configPath = std::string(temporary) + "funplugin-copy-test-" + std::to_string(GetCurrentProcessId()) + "\\";
    assert(CreateDirectoryA(configPath.c_str(), NULL));
    hInstance = GetModuleHandleW(NULL);
    assert(ts3plugin_init() == 0 && !g_copyWindow);
    assert(ensureCopyQueue() && g_copyWindow);
    delete g_copies;
    Copies::Hooks integratedHooks = testHooks();
    integratedHooks.ownership = copyOwnership; integratedHooks.opening = copyOpening;
    g_copies = new Copies::Manager(api, integratedHooks);
    PluginMenuItem** menus = nullptr; char* icon = nullptr;
    ts3plugin_initMenus(&menus, &icon);
    int menuCount = 0;
    for (; menus[menuCount]; ++menuCount) free(menus[menuCount]);
    free(menus); assert(menuCount == 20);
    assert(FindResourceW(hInstance, MAKEINTRESOURCEW(IDD_COPIES), MAKEINTRESOURCEW(5)));
    g.selfLockOn = 1; selfLock = 1; selfLockSch = 1; selfLockChannel = 9;
    assert(g_copies->start(request(1), GetTickCount64()));
    SendMessageW(g_copyWindow, WM_TIMER, 1, 0);
    assert(isCopyConnection(100));
    pump();
    assert(selfLockSch == 1);
    connections.at(100).status = STATUS_CONNECTION_ESTABLISHED;
    ts3plugin_onConnectStatusChangeEvent(100, STATUS_CONNECTION_ESTABLISHED, ERROR_ok);
    pump(); assert(selfLockSch == 1 && g_copies->count() == 1 && focused == 1);
    ts3plugin_onMenuItemEvent(1, PLUGIN_MENU_TYPE_GLOBAL, MENU_HARDSTOP, 0);
    assert(stops.size() == 1 && closed.size() == 1 && !g_copies->busy() && selfLockSch == 1);
    ts3plugin_onConnectStatusChangeEvent(100, STATUS_DISCONNECTED, ERROR_ok);
    pump(); assert(!isCopyConnection(100));
    ts3plugin_shutdown(); assert(!g_copyWindow && !g_copies);
    assert(RemoveDirectoryA(configPath.c_str()));
    std::cout << "PASS: no-audio profiles, 50-copy batch without device churn, audio guard, nested timers/Stop, count/Unicode, queue, errors, lifecycle and self-lock isolation\n";
}
