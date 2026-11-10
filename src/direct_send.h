#ifndef FUNPLUGIN_DIRECT_SEND_H
#define FUNPLUGIN_DIRECT_SEND_H

#include <windows.h>
#include <atomic>
#include <algorithm>
#include <string>
#include <vector>
#include "ts3_functions.h"
#include "teamspeak/public_errors.h"
#include "send_window.h"

namespace DirectSend {
struct Request {
    uint64 source = 0;
    anyID target = 0;
    std::vector<uint64> senders;
    int count = 1; // Per sender.
    bool privateMessage = false;
    std::string text, serverUID, targetUID;
};

inline std::string identity(TS3Functions& api, uint64 sch, anyID client = 0) {
    char* value = nullptr;
    unsigned int error = client
        ? api.getClientVariableAsString(sch, client, CLIENT_UNIQUE_IDENTIFIER, &value)
        : api.getServerVariableAsString(sch, VIRTUALSERVER_UNIQUE_IDENTIFIER, &value);
    std::string result;
    if (error == ERROR_ok && value) result = value;
    if (value) api.freeMemory(value);
    return result;
}

// One cancellable worker: no configured delay and no blocking send loop on the GUI.
class Batch {
    TS3Functions& api;
    SendWindow& window;
    uint64 epoch = 0;
    std::string pluginID;
    Request request;
    HANDLE thread = nullptr;
    CRITICAL_SECTION lock;
    std::atomic<bool> cancelled{false}, running{false};
    std::string returnCode; // Guarded with request by lock for SDK callbacks.

    bool connected(uint64 sch) {
        int status = STATUS_DISCONNECTED;
        return api.getConnectionStatus(sch, &status) == ERROR_ok && status == STATUS_CONNECTION_ESTABLISHED;
    }
    bool matches(uint64 sch) {
        return connected(sch) && identity(api, sch) == request.serverUID &&
               identity(api, sch, request.target) == request.targetUID;
    }
    static DWORD WINAPI entry(void* self) { ((Batch*)self)->run(); return 0; }
    void run() {
        uint64 submitted = 0;
        std::string reason;
        for (uint64 sender : request.senders) {
            for (int i = 0; i < request.count && !cancelled.load() && window.epoch() == epoch; ++i) {
                if (!matches(request.source) || !matches(sender)) {
                    reason = " Connection or recipient changed.";
                    cancelled = true;
                    break;
                }
                if (cancelled.load()) break;
                unsigned int error = window.send(epoch, sender, returnCode, pluginID.c_str(),
                    [&] { return cancelled.load(); }, [&](const char* code) {
                        if (!matches(request.source) || !matches(sender)) return (unsigned int)ERROR_client_invalid_id;
                        return request.privateMessage
                            ? api.requestSendPrivateTextMsg(sender, request.text.c_str(), request.target, code)
                            : api.requestClientPoke(sender, request.target, request.text.c_str(), code);
                    });
                if (error != ERROR_ok && error != ERROR_ok_no_update) {
                    reason = " TeamSpeak rejected a request (error " + std::to_string(error) + ").";
                    cancelled = true;
                    break;
                }
                ++submitted;
            }
            if (cancelled.load() || window.epoch() != epoch) { cancelled = true; break; }
        }
        std::string result = "[Fun Plugin] " + std::string(cancelled.load() ? "Send stopped: " : "Send finished: ") +
            std::to_string(submitted) + "/" + std::to_string((uint64)request.count * request.senders.size()) +
            " requests submitted to TeamSpeak. Server acceptance/delivery is not guaranteed." + reason;
        api.printMessage(request.source, result.c_str(), PLUGIN_MESSAGE_TARGET_SERVER);
        running = false;
    }
public:
    Batch(TS3Functions& functions, SendWindow& flow) : api(functions), window(flow) { InitializeCriticalSection(&lock); }
    ~Batch() { stop(); DeleteCriticalSection(&lock); }
    bool busy() const { return running.load(); }
    void stop() {
        cancelled = true;
        if (thread) { WaitForSingleObject(thread, INFINITE); CloseHandle(thread); thread = nullptr; }
    }
    bool start(const Request& value, const char* plugin) {
        if (busy() || value.count <= 0 || !value.source || !value.target || value.senders.empty() ||
            value.text.empty() || value.serverUID.empty() || value.targetUID.empty() ||
            !plugin || !api.createReturnCode) return false;
        stop();
        char code[128] = {};
        api.createReturnCode(plugin, code, sizeof(code));
        if (!code[0]) return false;
        EnterCriticalSection(&lock);
        request = value;
        pluginID = plugin;
        epoch = window.epoch();
        returnCode = code;
        cancelled = false;
        running = true;
        LeaveCriticalSection(&lock);
        thread = CreateThread(nullptr, 0, entry, this, 0, nullptr);
        if (!thread) { running = false; return false; }
        return true;
    }
    // Callback-safe; do not suppress TeamSpeak's own server error display.
    void reply(uint64 sch, const char* code, unsigned int error) {
        if (!code || error == ERROR_ok || error == ERROR_ok_no_update) return;
        EnterCriticalSection(&lock);
        if (!returnCode.empty() && returnCode == code &&
            std::find(request.senders.begin(), request.senders.end(), sch) != request.senders.end()) cancelled = true;
        LeaveCriticalSection(&lock);
    }
    void disconnected(uint64 sch) {
        EnterCriticalSection(&lock);
        if (sch == request.source || std::find(request.senders.begin(), request.senders.end(), sch) != request.senders.end())
            cancelled = true;
        LeaveCriticalSection(&lock);
    }
};
}
#endif
