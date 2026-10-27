#ifndef FUNPLUGIN_COPIES_H
#define FUNPLUGIN_COPIES_H

#include <climits>
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "ts3_functions.h"
#include "teamspeak/public_errors.h"

/* Driven on the GUI thread. SDK callbacks are queued there by plugin.cpp. */
namespace Copies {

// TS3's guiConnect profile normalizer recognizes this built-in sentinel as
// "None". Empty strings instead select the user's default hardware profiles.
// Verified against the installed 3.6.2 client; never create or edit user profiles.
inline const char* noAudioProfile() { return "__NONE__"; }

inline bool parseCount(const char* text, int& result) {
    if (!text || !*text) return false;
    unsigned int value = 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        unsigned int digit = (unsigned int)(*p - '0');
        if (value > ((unsigned int)INT_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (!value) return false;
    result = (int)value;
    return true;
}

/* TS3 limits nicknames to 30 UTF-8 characters, including the suffix. */
inline std::string nickname(const std::string& base, uint64 number) {
    std::string suffix = " #" + std::to_string(number);
    size_t limit = 30 - suffix.size(), end = 0, characters = 0;
    while (end < base.size() && characters < limit) {
        ++end;
        while (end < base.size() && ((unsigned char)base[end] & 0xc0) == 0x80) ++end;
        ++characters;
    }
    return (end ? base.substr(0, end) : "Copy") + suffix;
}

struct Request {
    uint64 source = 0, channel = 0;
    int count = 0;
    std::string address, serverPassword, channelPath, channelPassword, baseNickname;
};

struct Hooks {
    void (*ownership)(uint64, bool) = nullptr;
    void (*opening)(bool) = nullptr;
    bool (*beforeOpen)() = nullptr;
    bool (*afterOpen)(uint64) = nullptr;
    bool (*closeTab)(uint64) = nullptr;
};

class Manager {
    struct Copy {
        uint64 source = 0, channel = 0, restoreCapture = 0;
        uint64 started = 0;
        bool established = false, stopping = false, disconnected = false;
        int closeAttempts = 0;
    };
    TS3Functions& api;
    Hooks hooks;
    std::map<uint64, Copy> owned;
    Request batch;
    bool running = false;
    bool ticking = false;
    uint64 nextOpenAt = 0;
    static const uint64 settleMs = 500;
    std::set<uint64> pending;
    uint64 serial = 0;
    int joined = 0, launched = 0;
    bool cleanupWarning = false;

    static bool ok(unsigned int err) { return err == ERROR_ok || err == ERROR_ok_no_update; }

    void message(const std::string& text) {
        api.printMessage(batch.source, ("[Fun Plugin] " + text).c_str(), PLUGIN_MESSAGE_TARGET_SERVER);
    }

    void finish(const std::string& text) {
        running = false;
        message(text);
        batch.serverPassword.clear();
        batch.channelPassword.clear();
    }

    std::string progress() const {
        return std::to_string(joined) + "/" + std::to_string(batch.count) + " copies joined.";
    }

    void fail(const std::string& reason, unsigned int error = ERROR_ok) {
        std::string detail = reason;
        if (error) {
            char* description = nullptr;
            if (api.getErrorMessage(error, &description) == ERROR_ok && description) {
                detail += ": "; detail += description;
                api.freeMemory(description);
            }
        }
        std::vector<uint64> failed(pending.begin(), pending.end());
        pending.clear();
        finish("Copy queue stopped. " + progress() + " " + detail);
        for (uint64 sch : failed) stopOne(sch);
    }

    void stopOne(uint64 sch) {
        auto it = owned.find(sch);
        if (it == owned.end()) return;
        it->second.stopping = true;
        it->second.closeAttempts = 0;
        /* Keep ownership until DISCONNECTED, so self-lock cannot reclaim a copy. */
        uint64 capture = it->second.restoreCapture;
        unsigned int err = api.stopConnection(sch, "Fun Plugin: remove copy");
        restore(capture);
        int status = STATUS_DISCONNECTED;
        if (err == ERROR_not_connected || err == ERROR_server_invalid_id ||
            (api.getConnectionStatus(sch, &status) == ERROR_ok && status == STATUS_DISCONNECTED))
            it->second.disconnected = true;
    }

    void forget(uint64 sch) {
        owned.erase(sch);
        if (hooks.ownership) hooks.ownership(sch, false);
    }

    void cleanup(size_t limit = 2) {
        std::vector<uint64> ids;
        for (auto& entry : owned) {
            if (entry.second.stopping && !entry.second.disconnected) {
                int state = STATUS_DISCONNECTED;
                if (api.getConnectionStatus(entry.first, &state) != ERROR_ok || state == STATUS_DISCONNECTED)
                    entry.second.disconnected = true;
            }
            if (entry.second.disconnected && entry.second.closeAttempts < 20) ids.push_back(entry.first);
        }
        // Spread a large cleanup over GUI ticks and give deferred closes time
        // to finish. A stubborn tab must not block cleanup of newer entries.
        std::stable_sort(ids.begin(), ids.end(), [&](uint64 a, uint64 b) {
            return owned.at(a).closeAttempts < owned.at(b).closeAttempts;
        });
        if (ids.size() > limit) ids.resize(limit);
        for (uint64 sch : ids) {
            ++owned.at(sch).closeAttempts;
            if (hooks.closeTab && hooks.closeTab(sch)) forget(sch);
            else if (owned.at(sch).closeAttempts == 20 && !cleanupWarning) {
                cleanupWarning = true;
                message("TeamSpeak could not close a copy tab. Remove my copies retries cleanup.");
            }
        }
    }

    void restore(uint64 sch) {
        if (!sch || owns(sch)) return;
        int status = STATUS_DISCONNECTED;
        int active = HARDWAREINPUT_DISABLED;
        if (api.getConnectionStatus(sch, &status) == ERROR_ok && status == STATUS_CONNECTION_ESTABLISHED &&
            api.getClientSelfVariableAsInt(sch, CLIENT_INPUT_HARDWARE, &active) == ERROR_ok &&
            active != HARDWAREINPUT_ENABLED)
            api.activateCaptureDevice(sch);
    }

    bool silence(uint64 sch) {
        unsigned int mic = api.setClientSelfVariableAsInt(sch, CLIENT_INPUT_MUTED, MUTEINPUT_MUTED);
        unsigned int speaker = api.setClientSelfVariableAsInt(sch, CLIENT_OUTPUT_MUTED, MUTEOUTPUT_MUTED);
        unsigned int flush = api.flushClientSelfUpdates(sch, nullptr);
        return ok(mic) && ok(speaker) && ok(flush);
    }

    bool hasNoAudio(uint64 sch) {
        int input = HARDWAREINPUT_ENABLED, output = HARDWAREOUTPUT_ENABLED;
        return api.getClientSelfVariableAsInt(sch, CLIENT_INPUT_HARDWARE, &input) == ERROR_ok &&
               api.getClientSelfVariableAsInt(sch, CLIENT_OUTPUT_HARDWARE, &output) == ERROR_ok &&
               input == HARDWAREINPUT_DISABLED && output == HARDWAREOUTPUT_DISABLED;
    }

    void established(uint64 sch, uint64 now) {
        auto it = owned.find(sch);
        if (it == owned.end()) return;
        if (it->second.stopping) {
            silence(sch);
            restore(it->second.restoreCapture);
            api.stopConnection(sch, "Fun Plugin: copy queue cancelled");
            return;
        }
        if (it->second.established) return;
        bool muted = silence(sch);
        bool noAudio = hasNoAudio(sch);
        restore(it->second.restoreCapture);
        it->second.restoreCapture = 0;  // A later kick must not switch the user's microphone tab.
        anyID me = 0;
        uint64 channel = 0;
        if (!muted || !noAudio || api.getClientID(sch, &me) != ERROR_ok ||
            api.getChannelOfClient(sch, me, &channel) != ERROR_ok || channel != it->second.channel) {
            fail(!noAudio ? "TeamSpeak did not disable audio for the copy; the queue was stopped."
                         : muted ? "The copy could not join the requested channel (check its password/permissions)."
                                 : "The copy could not be muted.");
            return;
        }
        it->second.established = true;
        if (!pending.erase(sch)) return;
        ++joined;
        nextOpenAt = now + settleMs;
        if (joined == batch.count) finish(progress() + " Use Remove my copies to disconnect them.");
    }

public:
    explicit Manager(TS3Functions& functions, Hooks callbacks = {}) : api(functions), hooks(callbacks) {}
    bool owns(uint64 sch) const { return owned.count(sch) != 0; }
    bool busy() const { return running; }
    size_t count() const { return owned.size(); }

    bool start(const Request& request, uint64 now) {
        if (running || ticking || request.count <= 0 || !request.source || !request.channel ||
            request.address.empty() || request.channelPath.empty() || request.baseNickname.empty() || owns(request.source))
            return false;
        batch = request;
        running = true;
        pending.clear();
        joined = launched = 0;
        nextOpenAt = now;
        message("Joining " + std::to_string(batch.count) + " extra copies in the background. "
                "Remove my copies or HARD STOP cancels the queue.");
        return true;
    }

    void tick(uint64 now) {
        // Protect the tab snapshot if a client API pumps a nested GUI timer.
        if (ticking) return;
        struct TickGuard {
            bool& flag;
            explicit TickGuard(bool& value) : flag(value) { flag = true; }
            ~TickGuard() { flag = false; }
        } guard(ticking);
        cleanup();
        if (!running) return;
        int sourceStatus = STATUS_DISCONNECTED;
        if (api.getConnectionStatus(batch.source, &sourceStatus) != ERROR_ok ||
            sourceStatus != STATUS_CONNECTION_ESTABLISHED) {
            fail("The original connection disconnected.");
            return;
        }
        std::vector<uint64> waiting(pending.begin(), pending.end());
        for (uint64 sch : waiting) {
            if (!running) return;
            int status = STATUS_DISCONNECTED;
            unsigned int err = api.getConnectionStatus(sch, &status);
            if (err != ERROR_ok) fail("The new connection is unavailable.", err);
            else if (status == STATUS_CONNECTION_ESTABLISHED) established(sch, now);
            else if (now - owned.at(sch).started >= 20000) fail("The new connection timed out.");
        }
        for (const auto& entry : owned)
            if (entry.second.stopping && !entry.second.disconnected) return;
        // TS3 can deadlock creating a handler while another connection's
        // callbacks are registering it. Finish one handshake and let its
        // queued status callbacks settle before creating another GUI tab.
        if (running && pending.empty() && launched < batch.count && now >= nextOpenAt)
            openOne(now);
    }

    void openOne(uint64 now) {

        /* Snapshot pre-existing tabs; never take ownership of one of those. */
        uint64* handlers = nullptr;
        unsigned int err = api.getServerConnectionHandlerList(&handlers);
        if (err != ERROR_ok || !handlers) { fail("Cannot inspect current connections.", err); return; }
        std::set<uint64> previous;
        uint64 capture = 0;
        for (uint64* p = handlers; *p; ++p) {
            previous.insert(*p);
            int active = HARDWAREINPUT_DISABLED;
            if (!owns(*p) && api.getClientSelfVariableAsInt(*p, CLIENT_INPUT_HARDWARE, &active) == ERROR_ok &&
                active == HARDWAREINPUT_ENABLED) capture = *p;
        }
        api.freeMemory(handlers);
        if (hooks.beforeOpen && !hooks.beforeOpen()) {
            fail("Cannot preserve the current TeamSpeak tab.");
            return;
        }
        uint64 sch = 0;
        std::string nick = nickname(batch.baseNickname, ++serial);
        if (hooks.opening) hooks.opening(true);
        err = api.guiConnect(PLUGIN_CONNECT_TAB_NEW, "Fun Plugin copy", batch.address.c_str(),
                             batch.serverPassword.c_str(), nick.c_str(), batch.channelPath.c_str(),
                             batch.channelPassword.c_str(), noAudioProfile(), noAudioProfile(),
                             "", "", "", "", "", &sch);
        bool fresh = sch && !previous.count(sch);
        if (fresh) {
            Copy copy;
            copy.source = batch.source; copy.channel = batch.channel; copy.restoreCapture = capture;
            copy.started = now;
            owned.emplace(sch, copy);
            if (hooks.ownership) hooks.ownership(sch, true);
            pending.insert(sch);
            ++launched;
            /* Audio is disabled at creation; muting is an additional guard. */
            silence(sch);
        }
        bool tracked = !hooks.afterOpen || hooks.afterOpen(fresh ? sch : 0);
        if (hooks.opening) hooks.opening(false);
        restore(capture);
        // A stop click can arrive while guiConnect dispatches nested events.
        if (!running) { if (fresh) stopOne(sch); return; }
        if (err != ERROR_ok || !fresh) fail("TeamSpeak could not open another tab.", err);
        else if (!tracked) fail("Cannot track the new tab for automatic cleanup.");
    }

    void status(uint64 sch, int status, unsigned int error, uint64 now) {
        if (!owns(sch)) return;
        if (running) nextOpenAt = now + settleMs;
        if (status == STATUS_CONNECTION_ESTABLISHED) established(sch, now);
        else if (status == STATUS_DISCONNECTED) {
            uint64 capture = owned.at(sch).restoreCapture;
            bool failed = running && pending.count(sch);
            owned.at(sch).disconnected = true;
            restore(capture);
            if (failed) fail("TeamSpeak/server refused or closed the copy connection.", error);
        }
    }

    void stopAll() {
        running = false;
        pending.clear();
        cleanupWarning = false;
        batch.serverPassword.clear(); batch.channelPassword.clear();
        std::vector<uint64> ids;
        for (const auto& entry : owned) ids.push_back(entry.first);
        for (uint64 sch : ids) stopOne(sch);
        cleanup();
    }

    void shutdown() {
        stopAll();
        cleanup(owned.size()); // The timer will no longer run after unload.
        std::vector<uint64> ids;
        for (const auto& entry : owned) ids.push_back(entry.first);
        for (uint64 sch : ids) forget(sch);
    }
};
}
#endif
