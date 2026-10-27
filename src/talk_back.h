#ifndef FUNPLUGIN_TALK_BACK_H
#define FUNPLUGIN_TALK_BACK_H
#include <windows.h>
#include <atomic>
#include <array>
#include <cmath>
#include <algorithm>
#include <string>
#include "copies.h"

namespace TalkBack {
static const int rate = 48000;
struct Options {
    int semitones = -6;
    int volume = 75;
    bool valid() const { return semitones >= -12 && semitones <= 12 && semitones != 0 && volume >= 10 && volume <= 100; }
};

// Two crossfaded read heads shift pitch at a fixed 48 kHz output rate. The
// 43 ms delay window is constant: no phrase recording or waiting for silence.
class Pitch {
    std::array<float, 4096> history{};
    size_t write = 0;
    double phase = 0, increment = 0, gain = 0;
    float read(double delay) const {
        double position = (double)write - delay;
        if (position < 0) position += history.size();
        size_t a = (size_t)position, b = (a + 1) % history.size();
        return (float)(history[a] + (history[b] - history[a]) * (position - a));
    }
public:
    void reset(const Options& options) {
        history.fill(0); write = 0; phase = 0;
        increment = (1.0 - std::pow(2.0, options.semitones / 12.0)) / 2048.0;
        gain = options.volume / 100.0;
    }
    short process(short sample) {
        history[write] = sample;
        double other = phase < 0.5 ? phase + 0.5 : phase - 0.5;
        double weight = 0.5 - 0.5 * std::cos(6.283185307179586 * phase);
        double value = (read(64 + phase * 2048) * weight + read(64 + other * 2048) * (1 - weight)) * gain;
        write = (write + 1) % history.size();
        phase += increment;
        if (phase < 0) phase += 1;
        if (phase >= 1) phase -= 1;
        return (short)(std::max)(-32768.0, (std::min)(32767.0, value));
    }
};

// SDK callbacks only enqueue bounded selected-user PCM. The GUI's 20 ms
// timer runs DSP and feeds the custom capture device. The FIFO holds at most
// 200 ms; overflow drops old samples instead of accumulating playback delay.
class Audio {
    SRWLOCK lock = SRWLOCK_INIT;
    uint64 source = 0;
    anyID target = 0;
    bool active = false;
    std::array<short, rate / 5> fifo{};
    size_t readAt = 0, count = 0;
    int tail = 0;
    Pitch pitch;
    std::string expectedCode;
    unsigned int reply = 0;
    bool replyReady = false;
public:
    std::atomic<uint64> helper{0};
    std::atomic<bool> transmit{false};
    std::atomic<bool> cancelled{false};
    void arm(uint64 connection, anyID client, Options options) {
        AcquireSRWLockExclusive(&lock);
        source = connection; target = client; active = true;
        readAt = count = 0; tail = 0; pitch.reset(options); cancelled.store(false);
        ReleaseSRWLockExclusive(&lock);
    }
    void stop() {
        transmit.store(false);
        AcquireSRWLockExclusive(&lock);
        active = false; source = 0; target = 0; readAt = count = 0; tail = 0;
        expectedCode.clear(); replyReady = false;
        ReleaseSRWLockExclusive(&lock);
    }
    void receive(uint64 connection, anyID client, const short* samples, int frames, int channels) {
        if (!samples || frames <= 0 || frames > rate || channels < 1 || channels > 8) return;
        AcquireSRWLockExclusive(&lock);
        if (active && source == connection && target == client) {
            for (int i = 0; i < frames; ++i) {
                int sum = 0;
                for (int c = 0; c < channels; ++c) sum += samples[i * channels + c];
                if (count == fifo.size()) { readAt = (readAt + 1) % fifo.size(); --count; }
                fifo[(readAt + count) % fifo.size()] = (short)(sum / channels); ++count;
            }
        }
        ReleaseSRWLockExclusive(&lock);
    }
    bool render(short* output, int frames) {
        if (!output || frames <= 0 || frames > 960) return false;
        bool audible = false;
        AcquireSRWLockExclusive(&lock);
        for (int i = 0; i < frames; ++i) {
            short input = 0;
            if (active && count) {
                input = fifo[readAt]; readAt = (readAt + 1) % fifo.size(); --count;
                if (input > 16 || input < -16) tail = 2112;
            }
            bool hasAudio = active && tail > 0;
            if (hasAudio) --tail;
            output[i] = active ? pitch.process(input) : 0;
            audible = audible || hasAudio;
        }
        ReleaseSRWLockExclusive(&lock);
        return audible;
    }
    void departed(uint64 connection, anyID client) {
        AcquireSRWLockExclusive(&lock);
        if (active && source == connection && target == client) {
            active = false; count = 0; tail = 0; transmit.store(false); cancelled.store(true);
        }
        ReleaseSRWLockExclusive(&lock);
    }
    void disconnected(uint64 connection) {
        AcquireSRWLockExclusive(&lock);
        if (connection == source || connection == helper.load()) {
            active = false; count = 0; tail = 0; transmit.store(false); cancelled.store(true);
        }
        ReleaseSRWLockExclusive(&lock);
    }
    void expect(const std::string& code) {
        AcquireSRWLockExclusive(&lock);
        expectedCode = code; replyReady = false;
        ReleaseSRWLockExclusive(&lock);
    }
    bool serverReply(uint64 connection, const char* code, unsigned int error) {
        bool ours = false;
        AcquireSRWLockExclusive(&lock);
        if (connection == helper.load() && code && !expectedCode.empty() && expectedCode == code) {
            reply = error; replyReady = ours = true; expectedCode.clear();
        }
        ReleaseSRWLockExclusive(&lock);
        return ours;
    }
    bool takeReply(unsigned int& error) {
        AcquireSRWLockExclusive(&lock);
        bool ready = replyReady;
        if (ready) { error = reply; replyReady = false; }
        ReleaseSRWLockExclusive(&lock);
        return ready;
    }
};

class Session {
    TS3Functions& api;
    Audio& audio;
    Copies::Manager joins;
    uint64 source = 0, channel = 0, pausedCapture = 0, checkedAt = 0, configuredHelper = 0;
    anyID target = 0;
    std::string uid, device, returnCode, pluginIdentifier;
    Options options;
    bool registered = false, configured = false, ready = false;
    bool ticking = false, stopRequested = false, stopping = false;
    uint64 waitingSince = 0, nextFrame = 0, setupAfter = 0;
    unsigned long serial = 0;
    static bool ok(unsigned int error) { return error == ERROR_ok || error == ERROR_ok_no_update; }
    void message(const char* text) { if (source) api.printMessage(source, text, PLUGIN_MESSAGE_TARGET_SERVER); }
    bool check(unsigned int error, const char* stage) {
        if (ok(error)) return true;
        char* description = nullptr;
        std::string text = std::string("[Fun Plugin] Talk Back: ") + stage + " failed (" + std::to_string(error) + ")";
        if (api.getErrorMessage(error, &description) == ERROR_ok && description) {
            text += ": "; text += description; api.freeMemory(description);
        }
        message(text.c_str()); return false;
    }
    bool connected(uint64 connection) {
        int status = STATUS_DISCONNECTED;
        return api.getConnectionStatus(connection, &status) == ERROR_ok && status == STATUS_CONNECTION_ESTABLISHED;
    }
    uint64 activeCapture() {
        uint64* handlers = nullptr; uint64 result = 0;
        if (api.getServerConnectionHandlerList(&handlers) == ERROR_ok && handlers) {
            for (uint64* p = handlers; *p; ++p) {
                int active = HARDWAREINPUT_DISABLED;
                if (*p != audio.helper.load() && api.getClientSelfVariableAsInt(*p, CLIENT_INPUT_HARDWARE, &active) == ERROR_ok && active == HARDWAREINPUT_ENABLED) result = *p;
            }
            api.freeMemory(handlers);
        }
        return result;
    }
    void restore() {
        int helperActive = HARDWAREINPUT_DISABLED;
        uint64 helper = configuredHelper ? configuredHelper : audio.helper.load();
        bool ownsCapture = helper && api.getClientSelfVariableAsInt(helper, CLIENT_INPUT_HARDWARE, &helperActive) == ERROR_ok && helperActive == HARDWAREINPUT_ENABLED;
        if (pausedCapture && connected(pausedCapture) && (ownsCapture || !activeCapture())) api.activateCaptureDevice(pausedCapture);
        pausedCapture = 0;
    }
    bool validate() {
        uint64 targetChannel = 0, ownChannel = 0; anyID me = 0; char* identity = nullptr;
        bool valid = connected(source) && api.getClientID(source, &me) == ERROR_ok &&
            api.getChannelOfClient(source, me, &ownChannel) == ERROR_ok && ownChannel == channel &&
            api.getChannelOfClient(source, target, &targetChannel) == ERROR_ok && targetChannel == channel &&
            api.getClientVariableAsString(source, target, CLIENT_UNIQUE_IDENTIFIER, &identity) == ERROR_ok && identity && uid == identity;
        if (identity) api.freeMemory(identity);
        return valid;
    }
    bool configure(uint64 now) {
        uint64 helper = audio.helper.load();
        if (!helper || !connected(helper)) return false;
        configuredHelper = helper;
        device = "FunPluginTalkBack-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++serial);
        if (!check(api.registerCustomDevice(device.c_str(), "Fun Plugin Talk Back", rate, 1, rate, 1), "register synthetic input")) return false;
        registered = true;
        pausedCapture = activeCapture();
        bool opened = check(api.openCaptureDevice(helper, "custom", device.c_str()), "open synthetic input");
        restore();
        if (!opened) return false;
        if (!check(api.setClientSelfVariableAsInt(helper, CLIENT_INPUT_DEACTIVATED, INPUT_DEACTIVATED), "pause helper") ||
            !check(api.setClientSelfVariableAsInt(helper, CLIENT_INPUT_MUTED, MUTEINPUT_MUTED), "mute helper") ||
            !check(api.setClientSelfVariableAsInt(helper, CLIENT_OUTPUT_MUTED, MUTEOUTPUT_NONE), "enable helper connection") ||
            !check(api.flushClientSelfUpdates(helper, nullptr), "update helper")) return false;
        for (const char* flag : {"vad", "agc", "denoise", "echo_canceling"})
            if (!check(api.setPreProcessorConfigValue(helper, flag, "false"), flag)) return false;
        char code[128] = {}; api.createReturnCode(pluginIdentifier.c_str(), code, sizeof(code));
        if (!code[0]) return false;
        returnCode = code; audio.expect(returnCode);
        anyID clients[] = {target, 0}; uint64 channels[] = {0};
        if (!check(api.requestClientSetWhisperList(helper, 0, channels, clients, returnCode.c_str()), "set private whisper")) return false;
        configured = true; waitingSince = now; return true;
    }
public:
    Session(TS3Functions& functions, Audio& buffer, Copies::Hooks hooks, const char* identifier)
        : api(functions), audio(buffer), joins(functions, hooks), pluginIdentifier(identifier ? identifier : "") {}
    bool active() const { return source != 0; }
    bool connecting() const { return joins.busy(); }
    bool cleaning() const { return joins.count() != 0; }
    uint64 sourceConnection() const { return source; }
    anyID targetClient() const { return target; }
    bool start(Copies::Request request, anyID client, const std::string& identity, Options settings, uint64 now) {
        if (active() || cleaning() || stopping || !settings.valid() || !client || identity.empty() ||
            !api.registerCustomDevice || !api.unregisterCustomDevice || !api.openCaptureDevice ||
            !api.processCustomCaptureData || !api.setPreProcessorConfigValue || !api.requestClientSetWhisperList ||
            !api.createReturnCode || pluginIdentifier.empty()) return false;
        source = request.source; channel = request.channel; target = client; uid = identity; options = settings;
        checkedAt = now; setupAfter = 0; configured = ready = false;
        if (!validate()) { source = 0; return false; }
        request.count = 1; request.baseNickname = "Talk Back"; request.tabLabel = "Talk Back";
        if (!joins.start(request, now)) { stop(); return false; }
        message("[Fun Plugin] Talk Back is connecting. The live pitched echo whispers only to the selected user.");
        return true;
    }
    void status(uint64 connection, int state, unsigned int error, uint64 now) {
        joins.status(connection, state, error, now);
        if (connection == audio.helper.load() && state == STATUS_CONNECTION_ESTABLISHED && !configured) setupAfter = now + 500;
    }
    void stop() {
        audio.stop();
        if (ticking) { stopRequested = true; return; }
        if (stopping) return;
        stopping = true; stopRequested = false; source = 0; target = 0;
        uint64 helper = audio.helper.load();
        if (helper && registered) {
            api.setClientSelfVariableAsInt(helper, CLIENT_INPUT_DEACTIVATED, INPUT_DEACTIVATED);
            api.setClientSelfVariableAsInt(helper, CLIENT_INPUT_MUTED, MUTEINPUT_MUTED);
            api.flushClientSelfUpdates(helper, nullptr);
        }
        restore();
        if (registered) api.unregisterCustomDevice(device.c_str());
        registered = configured = ready = false; device.clear(); uid.clear(); configuredHelper = 0;
        joins.stopAll(); stopping = false;
    }
    void shutdown() { stop(); joins.shutdown(); }
    void tick(uint64 now) {
        if (ticking) return;
        struct Guard {
            Session& session;
            explicit Guard(Session& value) : session(value) { session.ticking = true; }
            ~Guard() { session.ticking = false; if (session.stopRequested) session.stop(); }
        } guard(*this);
        joins.tick(now);
        if (!active()) return;
        if (audio.cancelled.load() && ready) { stop(); return; }
        if (now >= checkedAt + 50) {
            checkedAt = now;
            if (!validate()) { message("[Fun Plugin] Talk Back stopped: the selected user or original connection changed."); stop(); return; }
        }
        if (joins.busy()) return;
        uint64 helper = audio.helper.load();
        if (!helper || !connected(helper)) { message("[Fun Plugin] Talk Back stopped: its connection closed."); stop(); return; }
        if (!configured) {
            if (!setupAfter) setupAfter = now + 500;
            if (now < setupAfter) return;
            if (!configure(now)) { message("[Fun Plugin] Talk Back could not set up its audio/whisper connection."); stop(); }
            return;
        }
        if (!ready) {
            unsigned int error = ERROR_ok;
            if (audio.takeReply(error)) {
                if (error != ERROR_ok) { message("[Fun Plugin] Talk Back stopped: the server rejected the private whisper."); stop(); return; }
                if (stopRequested) return;
                pausedCapture = activeCapture();
                if (!check(api.activateCaptureDevice(helper), "activate live echo") ||
                    !check(api.setClientSelfVariableAsInt(helper, CLIENT_INPUT_MUTED, MUTEINPUT_NONE), "unmute live echo") ||
                    !check(api.setClientSelfVariableAsInt(helper, CLIENT_INPUT_DEACTIVATED, INPUT_ACTIVE), "start live echo") ||
                    !check(api.flushClientSelfUpdates(helper, nullptr), "update live echo")) { stop(); return; }
                if (stopRequested) return;
                ready = true; audio.arm(source, target, options); nextFrame = now;
                message("[Fun Plugin] Talk Back LIVE: pitched audio is whispered as they speak. Your microphone returns when you stop Talk Back.");
            } else if (now >= waitingSince + 5000) { message("[Fun Plugin] Talk Back stopped: no whisper confirmation from the server."); stop(); }
            return;
        }
        int captureActive = HARDWAREINPUT_DISABLED;
        if (api.getClientSelfVariableAsInt(helper, CLIENT_INPUT_HARDWARE, &captureActive) != ERROR_ok || captureActive != HARDWAREINPUT_ENABLED) {
            message("[Fun Plugin] Talk Back stopped: you switched the active microphone connection."); stop(); return;
        }
        for (int frames = 0; frames < 3 && now >= nextFrame; ++frames) {
            short frame[960] = {};
            bool audible = audio.render(frame, 960);
            if (stopRequested || audio.cancelled.load()) return;
            audio.transmit.store(audible);
            if (!check(api.processCustomCaptureData(device.c_str(), frame, 960), "stream live audio")) { stop(); return; }
            if (stopRequested) return;
            nextFrame += 20;
        }
        if (nextFrame + 60 < now) nextFrame = now + 20;
    }
};
}
#endif
