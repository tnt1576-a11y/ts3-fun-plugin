/* Offline tests: synthetic voice, fake routing/mic; no server or audio device. */
#include <cassert>
#include <iostream>
#include "../src/plugin.cpp"
namespace {
TS3Functions fake = {};
int micMute, ptt, hardware, outputMute, targetChannel, whisperTarget, whisperRequests;
bool connected, priorWhisper, rejectWhisper, rejectClear, nestedStop;
std::string uid, vad, code;
TalkBack::Session* current = nullptr;
void print(uint64, const char*, PluginMessageTarget) {}
void printCurrent(const char*) {}
unsigned int release(void* p) { free(p); return ERROR_ok; }
unsigned int errorText(unsigned int, char** result) { *result = _strdup("fake rejection"); return ERROR_ok; }
unsigned int status(uint64 sch, int* result) { assert(sch == 1); *result = connected ? STATUS_CONNECTION_ESTABLISHED : STATUS_DISCONNECTED; return ERROR_ok; }
unsigned int client(uint64 sch, anyID* result) { assert(sch == 1); *result = 7; return ERROR_ok; }
unsigned int channel(uint64 sch, anyID who, uint64* result) { assert(sch == 1); *result = who == 42 ? targetChannel : 9; return ERROR_ok; }
unsigned int identity(uint64 sch, anyID who, size_t flag, char** result) {
    assert(sch == 1 && who == 42 && flag == CLIENT_UNIQUE_IDENTIFIER); *result = _strdup(uid.c_str()); return ERROR_ok;
}
unsigned int getInt(uint64 sch, size_t flag, int* result) {
    assert(sch == 1);
    if (flag == CLIENT_INPUT_MUTED) *result = micMute;
    else if (flag == CLIENT_INPUT_DEACTIVATED) *result = ptt;
    else if (flag == CLIENT_OUTPUT_MUTED) *result = outputMute;
    else if (flag == CLIENT_INPUT_HARDWARE) *result = hardware;
    else assert(false);
    return ERROR_ok;
}
void ensureMicrophoneMasked() {
    if (g_talkAudio.captureConnection.load() == 1 && !g_talkAudio.routeReady.load()) {
        short samples[480]; std::fill(samples, samples + 480, (short)30000); int edited = 2;
        ts3plugin_onEditCapturedVoiceDataEvent(1, samples, 480, 1, &edited);
        assert(!(edited & 2) && std::all_of(samples, samples + 480, [](short s) { return s == 0; }));
    }
}
unsigned int setInt(uint64 sch, size_t flag, int value) {
    assert(sch == 1); ensureMicrophoneMasked();
    if (flag == CLIENT_INPUT_MUTED) micMute = value;
    else if (flag == CLIENT_INPUT_DEACTIVATED) ptt = value;
    else assert(false); // Never alter speakers or open/close hardware.
    std::string text = std::to_string(value);
    ts3plugin_onClientSelfVariableUpdateEvent(sch, (int)flag, "", text.c_str());
    if (nestedStop) { nestedStop = false; current->stop(); }
    return ERROR_ok;
}
unsigned int flush(uint64 sch, const char*) { assert(sch == 1); return ERROR_ok_no_update; }
unsigned int getVad(uint64 sch, const char* flag, char** result) {
    assert(sch == 1 && std::string(flag) == "vad"); *result = _strdup(vad.c_str()); return ERROR_ok;
}
unsigned int setVad(uint64 sch, const char* flag, const char* value) {
    assert(sch == 1 && std::string(flag) == "vad"); ensureMicrophoneMasked(); vad = value; return ERROR_ok;
}
unsigned int isWhispering(uint64 sch, anyID me, int* result) { assert(sch == 1 && me == 7); *result = priorWhisper || whisperTarget; return ERROR_ok; }
void createCode(const char* id, char* result, size_t length) {
    assert(std::string(id) == "offline-test" && length >= 64); strcpy(result, "sdk-code");
}
unsigned int whisper(uint64 sch, anyID me, const uint64* channels, const anyID* clients, const char* returnCode) {
    assert(sch == 1 && me == 0); ensureMicrophoneMasked();
    if (!channels && !clients) {
        if (rejectClear) return ERROR_parameter_invalid;
        whisperTarget = 0; return ERROR_ok;
    }
    assert(channels && channels[0] == 0 && clients && clients[0] == 42 && clients[1] == 0);
    assert(returnCode && std::string(returnCode) == "sdk-code"); code = returnCode; ++whisperRequests;
    if (rejectWhisper) return ERROR_client_invalid_id;
    whisperTarget = 42;
    // Intentionally NO success callback: local whisper configuration must not time out.
    return ERROR_ok;
}
void reset() {
    g_talkAudio.stop(); micMute = ptt = hardware = 1; outputMute = 0; targetChannel = 9;
    whisperTarget = whisperRequests = 0; connected = true; priorWhisper = rejectWhisper = rejectClear = nestedStop = false;
    uid = "synthetic-user-uid"; vad = "true";
    fake = {};
    fake.getConnectionStatus = status; fake.getClientID = client; fake.getChannelOfClient = channel;
    fake.getClientVariableAsString = identity; fake.getClientSelfVariableAsInt = getInt;
    fake.setClientSelfVariableAsInt = setInt; fake.flushClientSelfUpdates = flush;
    fake.getPreProcessorConfigValue = getVad; fake.setPreProcessorConfigValue = setVad;
    fake.isWhispering = isWhispering; fake.requestClientSetWhisperList = whisper; fake.createReturnCode = createCode;
    fake.freeMemory = release; fake.getErrorMessage = errorText; fake.printMessage = print; fake.printMessageToCurrentTab = printCurrent;
    // No guiConnect, custom device, physical device, recording, or activation functions are supplied.
}
std::vector<short> sine(int seconds = 3) {
    std::vector<short> result(TalkBack::rate * seconds);
    for (size_t i = 0; i < result.size(); ++i) result[i] = (short)(12000 * std::sin(6.283185307179586 * 440 * i / TalkBack::rate));
    return result;
}
double frequency(const std::vector<short>& pcm) {
    size_t crossings = 0;
    for (size_t i = 1; i < pcm.size(); ++i) if (pcm[i - 1] < 0 && pcm[i] >= 0) ++crossings;
    return crossings * (double)TalkBack::rate / pcm.size();
}
void start(TalkBack::Session& session) {
    assert(session.start(1, 42, uid, TalkBack::Options(), 0));
    assert(session.active() && whisperTarget == 42 && !micMute && !ptt && vad == "false");
}
void restored() { assert(micMute == 1 && ptt == 1 && vad == "true" && !whisperTarget && !g_talkAudio.captureConnection.load()); }
}

int main() {
    auto voice = sine(); TalkBack::Options options;
    for (int semitones : {-12, 12}) {
        options.semitones = semitones; TalkBack::Pitch shift; shift.reset(options);
        std::vector<short> output;
        for (short sample : voice) output.push_back(shift.process(sample));
        assert(output.size() == voice.size()); output.erase(output.begin(), output.begin() + 4800);
        assert(std::abs(frequency(output) - (semitones < 0 ? 220 : 880)) < 30);
    }
    options.semitones = 0; assert(!options.valid()); options.semitones = -6;
    TalkBack::Audio audio; audio.arm(1, 42, options); audio.enable(); short output[960] = {};
    audio.receive(2, 42, voice.data(), 960, 1); audio.receive(1, 43, voice.data(), 960, 1);
    assert(!audio.render(output, 960));
    bool heardLive = false;
    for (int i = 0; i < 100; ++i) {
        audio.receive(1, 42, voice.data() + i * 960, 960, 1); assert(audio.render(output, 960));
        if (i <= 3 && std::any_of(output, output + 960, [](short s) { return s != 0; })) heardLive = true;
    }
    assert(heardLive); audio.stop(); assert(!audio.render(output, 960));
    audio.arm(1, 42, options); audio.enable();
    for (int i = 0; i < 100; ++i) audio.receive(1, 42, voice.data(), 960, 1);
    int drained = 0; while (audio.render(output, 960)) { ++drained; assert(drained <= 14); }
    assert(drained >= 10); audio.stop();

    reset();
    {
        TalkBack::Session session(fake, g_talkAudio, "offline-test"); start(session);
        session.tick(10000); assert(session.active()); // No success callback required, no 5-second timeout.
        short mic[960]; std::fill(mic, mic + 960, (short)30000); int edited = 2;
        ts3plugin_onEditCapturedVoiceDataEvent(1, mic, 960, 1, &edited);
        assert(!(edited & 2) && std::all_of(mic, mic + 960, [](short s) { return !s; }));
        bool live = false;
        for (int i = 0; i < 100; ++i) {
            ts3plugin_onEditPlaybackVoiceDataEvent(1, 42, voice.data() + i * 960, 960, 1);
            std::fill(mic, mic + 960, (short)30000); edited = 0;
            ts3plugin_onEditCapturedVoiceDataEvent(1, mic, 960, 1, &edited);
            assert(edited & 2);
            for (short s : mic) assert(std::abs((int)s) <= 9001); // Physical mic value 30000 never leaks.
            if (i <= 3 && std::any_of(mic, mic + 960, [](short s) { return s != 0; })) live = true;
        }
        assert(live && whisperRequests == 1);
        short unrelated[] = {30000, -30000}; edited = 2;
        ts3plugin_onEditCapturedVoiceDataEvent(2, unrelated, 2, 1, &edited);
        assert(unrelated[0] == 30000 && edited == 2);
        session.stop(); restored();
        ts3plugin_onEditCapturedVoiceDataEvent(1, unrelated, 2, 1, &edited);
        assert(unrelated[0] == 30000); // Normal microphone resumes only after route/settings restoration.
    }
    reset(); rejectWhisper = true;
    {
        TalkBack::Session session(fake, g_talkAudio, "offline-test");
        assert(!session.start(1, 42, uid, options, 0)); restored();
    }
    reset(); priorWhisper = true;
    {
        TalkBack::Session session(fake, g_talkAudio, "offline-test");
        assert(!session.start(1, 42, uid, options, 0) && !whisperRequests); restored();
    }
    reset(); hardware = 0;
    {
        TalkBack::Session session(fake, g_talkAudio, "offline-test");
        assert(!session.start(1, 42, uid, options, 0) && !whisperRequests); restored();
    }
    reset();
    {
        TalkBack::Session session(fake, g_talkAudio, "offline-test"); start(session);
        assert(ts3plugin_onServerPermissionErrorEvent(1, "", ERROR_permissions, code.c_str(), 0) == 1);
        assert(!g_talkAudio.routeReady.load()); session.tick(100); restored();
    }
    reset();
    {
        TalkBack::Session session(fake, g_talkAudio, "offline-test"); start(session);
        uid = "different-user-reusing-id"; session.tick(100); assert(!session.active()); restored();
    }
    reset();
    {
        TalkBack::Session session(fake, g_talkAudio, "offline-test"); current = &session; nestedStop = true;
        assert(!session.start(1, 42, uid, options, 0)); restored();
    }
    reset();
    {
        TalkBack::Session session(fake, g_talkAudio, "offline-test"); start(session);
        g_talkAudio.departed(1, 42); session.tick(100); restored();
    }
    reset(); micMute = ptt = 0;
    {
        TalkBack::Session session(fake, g_talkAudio, "offline-test"); start(session);
        ts3plugin_onClientSelfVariableUpdateEvent(1, CLIENT_INPUT_MUTED, "0", "1"); session.tick(100);
        assert(micMute == 1 && !ptt && !whisperTarget); // Respect the user's new mute choice.
    }
    reset();
    {
        TalkBack::Session session(fake, g_talkAudio, "offline-test"); start(session);
        InitializeCriticalSection(&cs); ts3Functions = fake; g_talkSession = &session;
        hardStop(); g_talkSession = nullptr; DeleteCriticalSection(&cs); restored();
    }
    reset();
    {
        TalkBack::Session session(fake, g_talkAudio, "offline-test"); start(session);
        rejectClear = true; session.stop(); assert(micMute == 1 && !g_talkAudio.captureConnection.load());
    }
    TalkBackDialog data; data.name = L"Synthetic friend"; hInstance = GetModuleHandleW(NULL);
    HWND dialog = CreateDialogParamW(hInstance, MAKEINTRESOURCEW(IDD_TALKBACK), NULL, talkBackProc, (LPARAM)&data);
    assert(dialog && IsDlgButtonChecked(dialog, IDC_TB_DOWN) == BST_CHECKED);
    assert(GetDlgItemInt(dialog, IDC_TB_STRENGTH, NULL, FALSE) == 6);
    assert(copyField(dialog, IDC_TB_TARGET) == "Synthetic friend" && !GetDlgItem(dialog, 1214)); DestroyWindow(dialog);
    std::cout << "PASS: live original-user echo, pitch up/down at original pace, no extra connection/device/recorder, no success callback wait, private routing, raw mic exclusion, bounded FIFO, failure/cancel/HARD STOP rollback, mute/PTT/VAD restoration, native dialog\n";
}
