#ifndef FUNPLUGIN_SEND_WINDOW_H
#define FUNPLUGIN_SEND_WINDOW_H
#include <windows.h>
#include <atomic>
#include <map>
#include <string>
#include "ts3_functions.h"
#include "teamspeak/public_errors.h"

// Shared accounting, with an independent bounded window for each connection.
class SendWindow {
    struct Pending { uint64 sch, started; std::string owner; };
    TS3Functions& api;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE changed;
    std::map<std::string, Pending> pending;
    std::atomic<uint64> generation{1};
    size_t count(uint64 sch) const {
        size_t n = 0;
        for (const auto& item : pending) if (item.second.sch == sch) ++n;
        return n;
    }
public:
    static const size_t capacity = 8;
    explicit SendWindow(TS3Functions& functions) : api(functions) {
        InitializeCriticalSection(&lock); InitializeConditionVariable(&changed);
    }
    ~SendWindow() { DeleteCriticalSection(&lock); }
    uint64 epoch() const { return generation.load(); }
    // An explicit Start may retry a previously timed-out connection.
    void restartConnection(uint64 sch) {
        EnterCriticalSection(&lock);
        uint64 now = GetTickCount64();
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->second.sch == sch && now - it->second.started >= 5000) it = pending.erase(it);
            else ++it;
        }
        WakeAllConditionVariable(&changed);
        LeaveCriticalSection(&lock);
    }
    void cancelAll() {
        EnterCriticalSection(&lock);
        ++generation; pending.clear(); WakeAllConditionVariable(&changed);
        LeaveCriticalSection(&lock);
    }
    std::string reply(uint64 sch, const char* code) {
        if (!code) return "";
        EnterCriticalSection(&lock);
        std::string owner;
        auto found = pending.find(code);
        if (found != pending.end() && found->second.sch == sch) {
            owner = found->second.owner; pending.erase(found); WakeAllConditionVariable(&changed);
        }
        LeaveCriticalSection(&lock);
        return owner;
    }
    template<class Cancelled, class Submit>
    unsigned int send(uint64 expectedEpoch, uint64 sch, const std::string& owner, const char* plugin,
                      Cancelled cancelled, Submit submit) {
        if (!plugin || !api.createReturnCode) return ERROR_not_connected;
        EnterCriticalSection(&lock);
        while (generation.load() == expectedEpoch && !cancelled() && count(sch) >= capacity) {
            uint64 now = GetTickCount64();
            bool timeout = false;
            for (const auto& item : pending)
                if (item.second.sch == sch && now - item.second.started >= 5000) timeout = true;
            if (timeout) {
                LeaveCriticalSection(&lock);
                if (api.printMessage) api.printMessage(sch, "[Fun Plugin] Sending stopped on this connection: no server acknowledgement for 5 seconds.", PLUGIN_MESSAGE_TARGET_SERVER);
                return ERROR_not_connected;
            }
            SleepConditionVariableCS(&changed, &lock, 10);
        }
        if (generation.load() != expectedEpoch || cancelled()) {
            LeaveCriticalSection(&lock); return ERROR_not_connected;
        }
        char code[128] = {};
        api.createReturnCode(plugin, code, sizeof(code));
        if (!*code || pending.count(code)) { LeaveCriticalSection(&lock); return ERROR_not_connected; }
        pending.emplace(code, Pending{sch, GetTickCount64(), owner});
        LeaveCriticalSection(&lock);
        if (generation.load() != expectedEpoch || cancelled()) { reply(sch, code); return ERROR_not_connected; }
        unsigned int error = submit(code);
        if (error != ERROR_ok && error != ERROR_ok_no_update) reply(sch, code);
        return error;
    }
};
#endif
