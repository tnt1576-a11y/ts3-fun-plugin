/* Offline audio and transport tests: synthetic PCM, fake SDK, no network/devices. */
#include <cassert>
#include <iostream>
#include "../src/plugin.cpp"

namespace {
TS3Functions fake = {};
int helperStatus, activeCapture, targetChannel, whisperMode, registered, stopped, closed;
int recording, recordingStarts, recordingStops, framesThisTick;
bool whisperConfirmed, nestedStop;
std::string targetUid, whisperCode;
std::vector<short> sent;
TalkBack::Session* sessionUnderTest = nullptr;
unsigned int release(void* pointer) { free(pointer); return ERROR_ok; }
void print(uint64, const char*, PluginMessageTarget) {}
void printCurrent(const char*) {}
unsigned int status(uint64 sch, int* value) {
    *value = sch == 1 ? STATUS_CONNECTION_ESTABLISHED : helperStatus; return ERROR_ok;
}
unsigned int handlers(uint64** result) {
    *result = (uint64*)calloc(3, sizeof(uint64)); (*result)[0] = 1;
    if (helperStatus != STATUS_DISCONNECTED) (*result)[1] = 100;
    return ERROR_ok;
}
unsigned int getInt(uint64 sch, size_t flag, int* value) {
    if (flag == CLIENT_INPUT_HARDWARE) *value = activeCapture == (int)sch;
    else if (flag == CLIENT_OUTPUT_HARDWARE) *value = sch == 1;
    else if (flag == CLIENT_IS_RECORDING) { assert(sch == 1); *value = recording; }
    else assert(false);
    return ERROR_ok;
}
unsigned int setInt(uint64 sch, size_t flag, int) {
    assert(sch == 100); // Never change the original's mute/PTT state.
    assert(flag == CLIENT_INPUT_MUTED || flag == CLIENT_OUTPUT_MUTED || flag == CLIENT_INPUT_DEACTIVATED);
    return ERROR_ok;
}
unsigned int flush(uint64 sch, const char*) { assert(sch == 100); return ERROR_ok_no_update; }
unsigned int client(uint64 sch, anyID* result) { *result = (anyID)sch; return ERROR_ok; }
unsigned int channel(uint64 sch, anyID who, uint64* result) {
    *result = sch == 1 && who == 42 ? targetChannel : 9; return ERROR_ok;
}
unsigned int identity(uint64 sch, anyID who, size_t flag, char** value) {
    assert(sch == 1 && who == 42 && flag == CLIENT_UNIQUE_IDENTIFIER);
    *value = _strdup(targetUid.c_str()); return ERROR_ok;
}
unsigned int activate(uint64 sch) { activeCapture = (int)sch; return ERROR_ok; }
unsigned int stop(uint64 sch, const char*) { assert(sch == 100); ++stopped; helperStatus = STATUS_DISCONNECTED; return ERROR_ok; }
unsigned int connect(PluginConnectTab tab, const char* label, const char*, const char*, const char* name,
    const char*, const char*, const char* cap, const char* play, const char*, const char*, const char*, const char*, const char*, uint64* result) {
    assert(tab == PLUGIN_CONNECT_TAB_NEW && std::string(label) == "Talk Back");
    assert(std::string(name) == "Talk Back #1");
    assert(std::string(cap) == "__NONE__" && std::string(play) == "__NONE__");
    *result = 100; helperStatus = STATUS_CONNECTING; return ERROR_ok;
}
unsigned int errorText(unsigned int, char** value) { *value = _strdup("fake error"); return ERROR_ok; }
unsigned int startRecording(uint64 sch) { assert(sch == 1); ++recordingStarts; recording = 1; return ERROR_ok; }
unsigned int stopRecording(uint64 sch) { assert(sch == 1); ++recordingStops; recording = 0; return ERROR_ok; }
unsigned int registerDevice(const char*, const char*, int capRate, int capChannels, int playRate, int playChannels) {
    assert(capRate == 48000 && playRate == 48000 && capChannels == 1 && playChannels == 1);
    ++registered; return ERROR_ok;
}
unsigned int unregisterDevice(const char*) { assert(registered == 1); --registered; return ERROR_ok; }
unsigned int openCapture(uint64 sch, const char* mode, const char*) {
    assert(sch == 100 && std::string(mode) == "custom"); activeCapture = 100; return ERROR_ok;
}
unsigned int preprocessor(uint64 sch, const char*, const char* value) {
    assert(sch == 100 && std::string(value) == "false"); return ERROR_ok;
}
unsigned int whisper(uint64 sch, anyID who, const uint64* channels, const anyID* clients, const char* code) {
    assert(sch == 100 && who == 0 && channels[0] == 0 && clients[0] == 42 && clients[1] == 0);
    whisperCode = code;
    assert(whisperCode == "sdk-generated-offline-return-code");
    if (whisperMode != 1) {
        whisperConfirmed = whisperMode == 0;
        if (whisperMode == 2) assert(ts3plugin_onServerPermissionErrorEvent(100, "", ERROR_permissions, code, 0) == 1);
        else assert(ts3plugin_onServerErrorEvent(100, "", ERROR_ok, code, "") == 1);
    }
    return ERROR_ok;
}
void createCode(const char* id, char* result, size_t length) {
    assert(std::string(id) == "offline-talk-back-test" && length >= 64);
    strcpy(result, "sdk-generated-offline-return-code");
}
unsigned int capture(const char*, const short* samples, int count) {
    assert(activeCapture == 100 && whisperConfirmed && count == 960);
    ++framesThisTick; assert(framesThisTick <= 3);
    short frame[960]; std::copy(samples, samples + count, frame); int edited = 2;
    ts3plugin_onEditCapturedVoiceDataEvent(100, frame, count, 1, &edited);
    if (edited & 2) sent.insert(sent.end(), frame, frame + count);
    else assert(std::all_of(frame, frame + count, [](short s) { return !s; }));
    if (nestedStop) {
        nestedStop = false;
        sessionUnderTest->tick(999999); // Nested GUI timer is ignored.
        sessionUnderTest->stop();
        assert(!g_talkAudio.transmit.load());
    }
    return ERROR_ok;
}
bool before() { return true; }
bool after(uint64) { return true; }
bool close(uint64 sch) { assert(sch == 100); ++closed; return true; }
void ownership(uint64 sch, bool owned) { g_talkAudio.helper.store(owned ? sch : 0); }
Copies::Hooks hooks() { Copies::Hooks h; h.beforeOpen = before; h.afterOpen = after; h.closeTab = close; h.ownership = ownership; return h; }
Copies::Request request() { Copies::Request r; r.source = 1; r.channel = 9; r.address = "offline.invalid:9987"; r.channelPath = "Test"; return r; }
void tick(TalkBack::Session& session, uint64 now) { framesThisTick = 0; session.tick(now); }
void reset() {
    g_talkAudio.stop(); g_talkAudio.helper.store(0);
    helperStatus = STATUS_DISCONNECTED; activeCapture = 1; targetChannel = 9;
    whisperMode = registered = stopped = closed = recording = recordingStarts = recordingStops = framesThisTick = 0;
    whisperConfirmed = nestedStop = false; targetUid = "synthetic-user-uid"; sent.clear();
    fake.getConnectionStatus = status; fake.getServerConnectionHandlerList = handlers;
    fake.getClientSelfVariableAsInt = getInt; fake.setClientSelfVariableAsInt = setInt;
    fake.flushClientSelfUpdates = flush; fake.getClientID = client; fake.getChannelOfClient = channel;
    fake.getClientVariableAsString = identity; fake.activateCaptureDevice = activate;
    fake.stopConnection = stop; fake.guiConnect = connect; fake.freeMemory = release;
    fake.getErrorMessage = errorText; fake.printMessage = print; fake.printMessageToCurrentTab = printCurrent;
    fake.startVoiceRecording = startRecording; fake.stopVoiceRecording = stopRecording; // Counters must remain zero.
    fake.registerCustomDevice = registerDevice; fake.unregisterCustomDevice = unregisterDevice;
    fake.openCaptureDevice = openCapture; fake.setPreProcessorConfigValue = preprocessor;
    fake.requestClientSetWhisperList = whisper; fake.processCustomCaptureData = capture;
    fake.createReturnCode = createCode;
}
void start(TalkBack::Session& session) {
    assert(session.start(request(), 42, targetUid, TalkBack::Options(), 0));
    tick(session, 0); assert(g_talkAudio.helper.load() == 100);
    helperStatus = STATUS_CONNECTION_ESTABLISHED;
    session.status(100, STATUS_CONNECTION_ESTABLISHED, ERROR_ok, 50);
    tick(session, 50); tick(session, 549); assert(activeCapture == 1 && !registered);
    tick(session, 550); assert(activeCapture == 1 && registered == 1);
    tick(session, 600);
}
std::vector<short> sine(int seconds = 1) {
    std::vector<short> result(TalkBack::rate * seconds);
    for (size_t i = 0; i < result.size(); ++i) result[i] = (short)(12000 * std::sin(6.283185307179586 * 440 * i / TalkBack::rate));
    return result;
}
double frequency(const std::vector<short>& pcm) {
    size_t crossings = 0;
    for (size_t i = 1; i < pcm.size(); ++i) if (pcm[i - 1] < 0 && pcm[i] >= 0) ++crossings;
    return crossings * (double)TalkBack::rate / pcm.size();
}
}

int main() {
    auto voice = sine(3);
    TalkBack::Options options;
    for (int semitones : {-12, 12}) {
        options.semitones = semitones;
        TalkBack::Pitch shifter; shifter.reset(options);
        std::vector<short> output;
        for (short sample : voice) output.push_back(shifter.process(sample));
        assert(output.size() == voice.size()); // Speaking pace never changes.
        output.erase(output.begin(), output.begin() + 4800);
        double expected = semitones < 0 ? 220 : 880;
        assert(std::abs(frequency(output) - expected) < 30);
    }
    options.semitones = 0; assert(!options.valid());
    options.semitones = 13; assert(!options.valid());
    options.semitones = -6;
    TalkBack::Audio audio;
    audio.arm(1, 42, options);
    short output[960] = {};
    audio.receive(2, 42, voice.data(), 960, 1);
    audio.receive(1, 43, voice.data(), 960, 1);
    assert(!audio.render(output, 960));
    bool heardLive = false;
    for (int i = 0; i < 100; ++i) {
        audio.receive(1, 42, voice.data() + i * 960, 960, 1);
        assert(audio.render(output, 960));
        if (i <= 3 && std::any_of(output, output + 960, [](short s) { return s != 0; })) heardLive = true;
    }
    assert(heardLive); // Audio appears during continuous speech, without any stop event.
    audio.stop(); assert(!audio.render(output, 960));
    audio.arm(1, 42, options);
    for (int i = 0; i < 100; ++i) audio.receive(1, 42, voice.data(), 960, 1);
    int drained = 0;
    while (audio.render(output, 960)) { ++drained; assert(drained <= 14); }
    assert(drained >= 10); // Fixed 200 ms backlog plus the shifter's short tail.
    audio.stop(); audio.arm(1, 42, options);
    std::vector<short> stereo(960 * 2);
    for (size_t i = 0; i < stereo.size(); i += 2) { stereo[i] = 1000; stereo[i + 1] = -1000; }
    audio.receive(1, 42, stereo.data(), 960, 2); audio.render(output, 960);
    assert(std::all_of(output, output + 960, [](short s) { return !s; }));
    audio.stop();

    reset();
    {
        TalkBack::Session session(fake, g_talkAudio, hooks(), "offline-talk-back-test"); start(session);
        assert(session.active() && activeCapture == 100);
        short original[] = {123, -321}; int edited = 2;
        ts3plugin_onEditCapturedVoiceDataEvent(1, original, 2, 1, &edited);
        assert(original[0] == 123 && edited == 2);
        tick(session, 620); assert(sent.empty()); // Silence never transmits.
        bool live = false;
        for (int i = 0; i < 100; ++i) {
            g_talkAudio.receive(1, 42, voice.data() + i * 960, 960, 1);
            tick(session, 640 + i * 20);
            if (i <= 3 && std::any_of(sent.begin(), sent.end(), [](short s) { return s != 0; })) live = true;
        }
        assert(live && activeCapture == 100 && !recordingStarts && !recordingStops);
        session.stop(); assert(!session.active() && activeCapture == 1 && !registered && closed == 1);
    }
    reset(); whisperMode = 1;
    {
        TalkBack::Session session(fake, g_talkAudio, hooks(), "offline-talk-back-test"); start(session);
        g_talkAudio.receive(1, 42, voice.data(), 960, 1);
        tick(session, 1000); assert(sent.empty() && activeCapture == 1);
        assert(!g_talkAudio.serverReply(1, whisperCode.c_str(), ERROR_ok));
        assert(!g_talkAudio.serverReply(100, "wrong-code", ERROR_ok));
        tick(session, 5600); assert(!session.active() && sent.empty() && !registered);
    }
    reset(); whisperMode = 2;
    {
        TalkBack::Session session(fake, g_talkAudio, hooks(), "offline-talk-back-test"); start(session);
        assert(!session.active() && sent.empty() && activeCapture == 1);
    }
    reset();
    {
        TalkBack::Session session(fake, g_talkAudio, hooks(), "offline-talk-back-test"); start(session);
        g_talkAudio.receive(1, 42, voice.data(), 960, 1);
        targetUid = "different-user-reusing-client-id";
        tick(session, 660); assert(!session.active() && sent.empty());
    }
    reset();
    {
        TalkBack::Session session(fake, g_talkAudio, hooks(), "offline-talk-back-test"); start(session);
        sessionUnderTest = &session; nestedStop = true;
        g_talkAudio.receive(1, 42, voice.data(), 960, 1);
        tick(session, 620);
        assert(!session.active() && activeCapture == 1 && !registered && closed == 1);
    }
    reset();
    {
        TalkBack::Session session(fake, g_talkAudio, hooks(), "offline-talk-back-test"); start(session);
        helperStatus = STATUS_DISCONNECTED; activeCapture = 0;
        session.status(100, STATUS_DISCONNECTED, ERROR_ok, 610); tick(session, 620);
        assert(!session.active() && activeCapture == 1 && !registered);
    }
    reset();
    {
        TalkBack::Session session(fake, g_talkAudio, hooks(), "offline-talk-back-test"); start(session);
        InitializeCriticalSection(&cs); ts3Functions = fake; g_talkSession = &session;
        hardStop(); g_talkSession = nullptr; DeleteCriticalSection(&cs);
        assert(!session.active() && !registered && activeCapture == 1 && closed == 1);
    }
    reset();
    {
        TalkBack::Session session(fake, g_talkAudio, hooks(), "offline-talk-back-test"); start(session);
        g_talkAudio.departed(1, 42); tick(session, 620);
        assert(!session.active() && sent.empty());
    }
    TalkBackDialog data; data.name = L"Synthetic friend";
    hInstance = GetModuleHandleW(NULL);
    HWND dialog = CreateDialogParamW(hInstance, MAKEINTRESOURCEW(IDD_TALKBACK), NULL, talkBackProc, (LPARAM)&data);
    assert(dialog && IsDlgButtonChecked(dialog, IDC_TB_DOWN) == BST_CHECKED);
    assert(GetDlgItemInt(dialog, IDC_TB_STRENGTH, NULL, FALSE) == 6);
    assert(copyField(dialog, IDC_TB_TARGET) == "Synthetic friend" && !GetDlgItem(dialog, 1214));
    DestroyWindow(dialog);
    std::cout << "PASS: live streaming during continuous speech, pitch up/down at original pace, bounded latency/FIFO, stereo, private whisper SDK return-code/ACK/rejection/timeout, no recorder, capture restoration, recycled IDs, nested Stop/HARD STOP, helper closure, native dialog\n";
}
