#ifndef FUNPLUGIN_TALK_BACK_H
#define FUNPLUGIN_TALK_BACK_H
#include <windows.h>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <map>
#include <vector>
#include <bitset>
#include "copies.h"

namespace TalkBack {
static const int rate = 48000;
enum class Mode { Private, Channel };
struct Member { anyID id; bool whisper; };
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

// The current user's microphone callback is the audio clock. It replaces all
// microphone samples with incoming voice; no other connection is created.
class Audio {
    SRWLOCK lock = SRWLOCK_INIT;
    uint64 source = 0;
    anyID target = 0;
    anyID self = 0;
    Mode mode = Mode::Private;
    bool active = false;
    struct Input {
        std::array<short, rate / 5> fifo{};
        size_t readAt = 0, count = 0;
        bool whisper = false;
        void clear() { readAt = count = 0; }
        void push(short sample) {
            if (count == fifo.size()) { readAt = (readAt + 1) % fifo.size(); --count; }
            fifo[(readAt + count) % fifo.size()] = sample; ++count;
        }
    } input;
    // Membership and storage are prepared outside the audio callbacks. Each
    // speaker has a bounded FIFO, so simultaneous voices mix at the same pace.
    std::map<anyID, Input> members;
    int tail = 0;
    Pitch pitch;
    std::string expectedCode;
public:
    std::atomic<uint64> captureConnection{0};
    std::atomic<bool> routeReady{false}, cancelled{false}, userMuted{false}, userPttDisabled{false};
    void arm(uint64 connection, anyID client, Options options, Mode selected = Mode::Private, anyID me = 0) {
        AcquireSRWLockExclusive(&lock);
        source = connection; target = client; self = me; mode = selected; active = true;
        input.clear(); members.clear(); tail = 0; pitch.reset(options);
        cancelled.store(false); userMuted.store(false); userPttDisabled.store(false);
        routeReady.store(false); captureConnection.store(connection);
        ReleaseSRWLockExclusive(&lock);
    }
    void enable() { if (!cancelled.load()) routeReady.store(true); }
    void mute() {
        routeReady.store(false);
        AcquireSRWLockExclusive(&lock);
        active = false; input.clear(); members.clear(); tail = 0;
        ReleaseSRWLockExclusive(&lock);
    }
    void stop() {
        mute();
        AcquireSRWLockExclusive(&lock);
        source = 0; target = 0; expectedCode.clear(); captureConnection.store(0);
        ReleaseSRWLockExclusive(&lock);
    }
    void setMembers(uint64 connection, const std::vector<Member>& list) {
        std::bitset<65536> present;
        for (const auto& member : list) present.set(member.id);
        AcquireSRWLockExclusive(&lock);
        if (active && source == connection && mode == Mode::Channel) {
            for (auto it = members.begin(); it != members.end();) {
                if (!present.test(it->first)) it = members.erase(it); else ++it;
            }
            for (const auto& member : list) if (member.id && member.id != self) {
                auto found = members.find(member.id);
                if (found == members.end()) {
                    found = members.emplace(std::piecewise_construct,
                        std::forward_as_tuple(member.id), std::forward_as_tuple()).first;
                    found->second.whisper = member.whisper;
                } else if (member.whisper) { found->second.whisper = true; found->second.clear(); }
            }
        }
        ReleaseSRWLockExclusive(&lock);
    }
    void talkStatus(uint64 connection, anyID client, bool whisper) {
        AcquireSRWLockExclusive(&lock);
        if (active && source == connection && mode == Mode::Channel) {
            auto found = members.find(client);
            if (found != members.end()) {
                if (found->second.whisper != whisper) found->second.clear();
                found->second.whisper = whisper;
            }
        }
        ReleaseSRWLockExclusive(&lock);
    }
    void receive(uint64 connection, anyID client, const short* samples, int frames, int channels) {
        if (!samples || frames <= 0 || frames > rate || channels < 1 || channels > 8) return;
        AcquireSRWLockExclusive(&lock);
        Input* incoming = nullptr;
        if (active && source == connection && routeReady.load()) {
            if (mode == Mode::Private && target == client) incoming = &input;
            if (mode == Mode::Channel && client != self) {
                auto found = members.find(client);
                if (found != members.end() && !found->second.whisper) incoming = &found->second;
            }
        }
        if (incoming) {
            for (int i = 0; i < frames; ++i) {
                int sum = 0;
                for (int c = 0; c < channels; ++c) sum += samples[i * channels + c];
                incoming->push((short)(sum / channels));
            }
        }
        ReleaseSRWLockExclusive(&lock);
    }
    bool render(short* output, int frames) {
        if (!output || frames <= 0 || frames > 960) return false;
        bool audible = false;
        AcquireSRWLockExclusive(&lock);
        int mixed[960] = {};
        bool enabled = active && routeReady.load() && !cancelled.load();
        auto mix = [&](Input& incoming) {
            size_t amount = (std::min)((size_t)frames, incoming.count);
            for (size_t i = 0; i < amount; ++i) {
                mixed[i] += incoming.fifo[incoming.readAt];
                incoming.readAt = (incoming.readAt + 1) % incoming.fifo.size();
            }
            incoming.count -= amount;
        };
        if (enabled) {
            if (mode == Mode::Private) mix(input);
            else for (auto& member : members) if (!member.second.whisper) mix(member.second);
        }
        for (int i = 0; i < frames; ++i) {
            short sample = (short)(std::max)(-32768, (std::min)(32767, mixed[i]));
            if (sample > 16 || sample < -16) tail = 2112;
            bool hasAudio = enabled && tail > 0;
            if (hasAudio) --tail;
            output[i] = enabled ? pitch.process(sample) : 0;
            audible = audible || hasAudio;
        }
        ReleaseSRWLockExclusive(&lock);
        return audible;
    }
    void captured(uint64 connection, short* samples, int frames, int channels, int* edited) {
        if (connection != captureConnection.load() || !samples || !edited || frames <= 0 || frames > rate || channels < 1 || channels > 8) return;
        bool audible = false;
        for (int offset = 0; offset < frames; offset += 960) {
            short mono[960] = {};
            int amount = (std::min)(960, frames - offset);
            audible = render(mono, amount) || audible;
            for (int i = 0; i < amount; ++i)
                for (int c = 0; c < channels; ++c) samples[(offset + i) * channels + c] = mono[i];
        }
        *edited = (*edited | 1) & ~2;
        if (audible && routeReady.load() && !cancelled.load()) *edited |= 2;
    }
    void cancel() { routeReady.store(false); cancelled.store(true); }
    void departed(uint64 connection, anyID client) {
        AcquireSRWLockExclusive(&lock);
        if (active && source == connection) {
            if (client == self || (mode == Mode::Private && target == client)) {
                active = false; input.clear(); tail = 0; cancel();
            } else if (mode == Mode::Channel) {
                members.erase(client);
            }
        }
        ReleaseSRWLockExclusive(&lock);
    }
    void disconnected(uint64 connection) { if (connection == captureConnection.load()) cancel(); }
    void selfUpdate(uint64 connection, int flag, const char* value) {
        if (connection != captureConnection.load() || !routeReady.load() || !value || atoi(value) != 1) return;
        if (flag == CLIENT_INPUT_MUTED) { userMuted.store(true); cancel(); }
        if (flag == CLIENT_INPUT_DEACTIVATED) { userPttDisabled.store(true); cancel(); }
        if (flag == CLIENT_OUTPUT_MUTED) cancel();
    }
    void expect(const std::string& code) {
        AcquireSRWLockExclusive(&lock); expectedCode = code; ReleaseSRWLockExclusive(&lock);
    }
    bool serverReply(uint64 connection, const char* code, unsigned int error) {
        bool ours = false;
        AcquireSRWLockExclusive(&lock);
        if (connection == source && code && !expectedCode.empty() && expectedCode == code) {
            ours = true; expectedCode.clear(); if (error != ERROR_ok) cancel();
        }
        ReleaseSRWLockExclusive(&lock);
        return ours;
    }
};

class Session {
    TS3Functions& api;
    Audio& audio;
    uint64 source = 0, channel = 0, checkedAt = 0;
    anyID target = 0, self = 0;
    Mode mode = Mode::Private;
    std::string uid, savedVad, pluginIdentifier;
    int savedMute = 0, savedPtt = 0;
    bool whisperOwned = false, operating = false, stopRequested = false, stopping = false;
    static bool ok(unsigned int error) { return error == ERROR_ok || error == ERROR_ok_no_update; }
    struct Guard {
        Session& session;
        explicit Guard(Session& s) : session(s) { session.operating = true; }
        ~Guard() { session.operating = false; if (session.stopRequested) session.stop(); }
    };
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
    bool validate() {
        int status = STATUS_DISCONNECTED; uint64 targetChannel = 0, ownChannel = 0; anyID me = 0; char* identity = nullptr;
        bool valid = api.getConnectionStatus(source, &status) == ERROR_ok && status == STATUS_CONNECTION_ESTABLISHED &&
            api.getClientID(source, &me) == ERROR_ok && me != target &&
            api.getChannelOfClient(source, me, &ownChannel) == ERROR_ok && ownChannel == channel;
        if (valid && mode == Mode::Private) valid =
            api.getChannelOfClient(source, target, &targetChannel) == ERROR_ok && targetChannel == channel &&
            api.getClientVariableAsString(source, target, CLIENT_UNIQUE_IDENTIFIER, &identity) == ERROR_ok && identity && uid == identity;
        if (identity) api.freeMemory(identity);
        return valid;
    }
    bool refreshMembers() {
        if (mode != Mode::Channel) return true;
        anyID* clients = nullptr;
        if (api.getChannelClientList(source, channel, &clients) != ERROR_ok || !clients) return false;
        std::vector<Member> list;
        for (anyID* client = clients; *client; ++client) {
            if (*client == self) continue;
            int whisper = 1;
            // Never expose received private whispers to the channel, including
            // whispers already in progress when the effect is started.
            if (api.isReceivingWhisper(source, *client, &whisper) != ERROR_ok) whisper = 1;
            list.push_back({*client, whisper != 0});
        }
        api.freeMemory(clients);
        audio.setMembers(source, list);
        return true;
    }
public:
    Session(TS3Functions& functions, Audio& buffer, const char* identifier)
        : api(functions), audio(buffer), pluginIdentifier(identifier ? identifier : "") {}
    bool active() const { return source != 0; }
    uint64 sourceConnection() const { return source; }
    anyID targetClient() const { return target; }
    bool channelMode() const { return mode == Mode::Channel; }
    bool startChannel(uint64 connection, uint64 ownChannel, Options options, uint64 now) {
        return start(connection, 0, "", options, now, Mode::Channel, ownChannel);
    }
    bool start(uint64 connection, anyID client, const std::string& identity, Options options, uint64 now,
               Mode selected = Mode::Private, uint64 expectedChannel = 0) {
        if (active() || operating || stopping || !options.valid() || !connection ||
            (selected == Mode::Private && (!client || identity.empty())) ||
            (selected == Mode::Channel && (!expectedChannel || !api.getChannelClientList || !api.isReceivingWhisper)) ||
            !api.requestClientSetWhisperList || !api.getPreProcessorConfigValue || !api.setPreProcessorConfigValue || !api.isWhispering) return false;
        anyID me = 0; uint64 ownChannel = 0; int active = 0, outputMuted = 0, whispering = 0;
        int inputMute = 0, ptt = 0; char* vad = nullptr;
        if (api.getClientID(connection, &me) != ERROR_ok || me == client ||
            api.getChannelOfClient(connection, me, &ownChannel) != ERROR_ok ||
            api.getClientSelfVariableAsInt(connection, CLIENT_INPUT_HARDWARE, &active) != ERROR_ok || !active ||
            api.getClientSelfVariableAsInt(connection, CLIENT_OUTPUT_MUTED, &outputMuted) != ERROR_ok || outputMuted ||
            api.getClientSelfVariableAsInt(connection, CLIENT_INPUT_MUTED, &inputMute) != ERROR_ok ||
            api.getClientSelfVariableAsInt(connection, CLIENT_INPUT_DEACTIVATED, &ptt) != ERROR_ok ||
            api.isWhispering(connection, me, &whispering) != ERROR_ok || whispering) return false;
        if (api.getPreProcessorConfigValue(connection, "vad", &vad) != ERROR_ok || !vad) return false;
        savedVad = vad; api.freeMemory(vad);
        if (selected == Mode::Channel && ownChannel != expectedChannel) return false;
        source = connection; target = client; self = me; channel = ownChannel; uid = identity; mode = selected;
        savedMute = inputMute; savedPtt = ptt; checkedAt = now;
        if (!validate()) { source = 0; return false; }
        Guard guard(*this);
        audio.arm(source, target, options, mode, me); // Mask microphone BEFORE changing routing or mute/PTT.
        if (!refreshMembers()) { stopRequested = true; return false; }
        char code[128] = {};
        if (api.createReturnCode && !pluginIdentifier.empty()) api.createReturnCode(pluginIdentifier.c_str(), code, sizeof(code));
        if (mode == Mode::Private) audio.expect(code);
        uint64 channels[] = {0}; anyID clients[] = {target, 0}; whisperOwned = true;
        // This sets local outgoing voice routing. ERROR_ok is sufficient; TS3
        // does not always issue a later success callback for this operation.
        unsigned int routed = mode == Mode::Private
            ? api.requestClientSetWhisperList(source, 0, channels, clients, code[0] ? code : nullptr)
            : api.requestClientSetWhisperList(source, 0, nullptr, nullptr, nullptr);
        if (!check(routed, mode == Mode::Private ? "set private whisper" : "set channel voice") ||
            !check(api.setPreProcessorConfigValue(source, "vad", "false"), "enable echo input") ||
            !check(api.setClientSelfVariableAsInt(source, CLIENT_INPUT_MUTED, MUTEINPUT_NONE), "unmute echo") ||
            !check(api.setClientSelfVariableAsInt(source, CLIENT_INPUT_DEACTIVATED, INPUT_ACTIVE), "enable echo") ||
            !check(api.flushClientSelfUpdates(source, nullptr), "update echo")) { stopRequested = true; return false; }
        if (stopRequested || audio.cancelled.load()) { stopRequested = true; return false; }
        audio.enable();
        message(mode == Mode::Private
            ? "[Fun Plugin] Talk Back LIVE through your user: pitched echo whispers only to the selected person. Stop restores your microphone."
            : "[Fun Plugin] Channel Talk Back LIVE through your user: everyone in your channel hears the pitched echo as normal voice. Stop restores your microphone.");
        return true;
    }
    void stop() {
        audio.mute(); // Keep zeroing microphone buffers until routing and settings are restored.
        if (operating) { stopRequested = true; return; }
        if (stopping) return;
        stopping = true; stopRequested = false;
        uint64 previous = source; source = 0; target = 0;
        if (previous) {
            bool cleared = !whisperOwned || ok(api.requestClientSetWhisperList(previous, 0, nullptr, nullptr, nullptr));
            api.setPreProcessorConfigValue(previous, "vad", savedVad.c_str());
            api.setClientSelfVariableAsInt(previous, CLIENT_INPUT_DEACTIVATED, audio.userPttDisabled.load() ? INPUT_DEACTIVATED : savedPtt);
            api.setClientSelfVariableAsInt(previous, CLIENT_INPUT_MUTED, cleared && !audio.userMuted.load() ? savedMute : MUTEINPUT_MUTED);
            api.flushClientSelfUpdates(previous, nullptr);
            if (!cleared) api.printMessage(previous, "[Fun Plugin] Talk Back stopped, but the whisper route could not be cleared. Your microphone remains muted.", PLUGIN_MESSAGE_TARGET_SERVER);
        }
        whisperOwned = false; uid.clear(); savedVad.clear(); audio.stop(); stopping = false;
    }
    void shutdown() { stop(); }
    void tick(uint64 now) {
        if (!active() || operating) return;
        if (audio.cancelled.load()) { message("[Fun Plugin] Talk Back stopped: routing, connection or microphone state changed."); stop(); return; }
        if (now < checkedAt + 100) return;
        Guard guard(*this); // SDK cache reads may dispatch a nested Stop event.
        checkedAt = now;
        int activeInput = 0, whispering = 0;
        if (!validate() || api.getClientSelfVariableAsInt(source, CLIENT_INPUT_HARDWARE, &activeInput) != ERROR_ok || !activeInput ||
            (mode == Mode::Channel && (api.isWhispering(source, self, &whispering) != ERROR_ok || whispering)) || !refreshMembers()) {
            message("[Fun Plugin] Talk Back stopped: the channel, selected user, voice route or active connection changed."); stop();
        }
    }
};
}
#endif
