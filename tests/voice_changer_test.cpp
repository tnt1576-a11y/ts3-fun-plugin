// Offline DSP and exported-callback tests. No microphone, server, or real config.
#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>
#include "../src/plugin.cpp"

static std::vector<short> tone(double frequency, int frames = VoiceChanger::rate * 2) {
    std::vector<short> samples(frames);
    for (int i = 0; i < frames; ++i) samples[i] = (short)(12000 * std::sin(2 * VoiceChanger::pi * frequency * i / VoiceChanger::rate));
    return samples;
}
static double crossingFrequency(const std::vector<short>& samples) {
    int first = -1, last = 0, count = 0;
    for (int i = VoiceChanger::rate / 2; i < (int)samples.size(); ++i) {
        if (samples[i - 1] <= 0 && samples[i] > 0) {
            if (first < 0) first = i;
            last = i; ++count;
        }
    }
    assert(count > 1); return (count - 1) * (double)VoiceChanger::rate / (last - first);
}
static void print(const char*) {}
static std::string config;
static void configPath(char* buffer, size_t size) { assert(config.size() < size); strcpy(buffer, config.c_str()); }
static unsigned int status(uint64, int* state) { *state = STATUS_CONNECTION_ESTABLISHED; return ERROR_ok; }
static std::vector<short> processed(VoiceChanger::Options options, const std::vector<short>& input, int block = 480) {
    VoiceChanger::Audio audio;
    assert(audio.start(1, options, audio.generation()));
    auto output = input;
    for (int offset = 0; offset < (int)output.size(); offset += block) {
        int flags = 2 | 16;
        audio.captured(1, output.data() + offset, (std::min)(block, (int)output.size() - offset), 1, &flags);
        assert(flags == (3 | 16));
    }
    return output;
}
int main() {
    _set_error_mode(_OUT_TO_STDERR);
    using namespace VoiceChanger;
    Options options;
    auto input = tone(220);
    auto startTime = std::chrono::steady_clock::now();
    std::vector<std::vector<short>> voices;
    for (int preset = 0; preset < Count; ++preset) {
        options.preset = preset;
        auto output = processed(options, input);
        assert(output != input);
        long long energy = 0;
        for (short value : output) energy += (long long)value * value;
        assert(energy > 10000000);
        // Different callback sizes must produce the same continuous signal.
        assert(output == processed(options, input, 137));
        for (const auto& earlier : voices) assert(output != earlier);
        voices.push_back(output);
        Processor processor; processor.reset(options);
        for (int i = 0; i < 6000; ++i) assert(processor.process(0) == 0);
        for (int i = 0; i < 10000; ++i) processor.process(i % 2 ? 32767 : -32768);
        short tail = 0;
        for (int i = 0; i < 10000; ++i) tail = processor.process(0);
        assert(tail == 0);
    }
    for (const auto& expected : std::vector<std::pair<int, int>>{{Female, 5}, {Deep, -7}, {Baby, 9}, {Chipmunk, 12}}) {
        double frequency = crossingFrequency(voices[expected.first]);
        double target = 220 * std::pow(2.0, expected.second / 12.0);
        std::cout << "Pitch " << expected.first << ": " << frequency << " Hz (target " << target << ")\n";
        assert(std::abs(frequency / target - 1) < 0.04);
    }
    for (double frequency : {82.0, 113.0, 224.0, 307.0, 440.0}) {
        NoteDetector detector;
        double ratio = 1;
        for (short value : tone(frequency, rate / 4)) ratio = detector.process(value / 32768.0);
        double note = 69 + 12 * std::log2(frequency / 440.0);
        double target = 440 * std::pow(2.0, (std::round(note) - 69) / 12.0);
        assert(std::abs(frequency * ratio - target) < 0.8);
    }
    options.preset = AutoTune;
    auto tuned = processed(options, tone(224));
    double tunedFrequency = crossingFrequency(tuned);
    std::cout << "Auto-tune 224 Hz -> " << tunedFrequency << " Hz (target 220)\n";
    assert(std::abs(tunedFrequency - 220) < 1.5);
    auto hardTuned = processed(options, tone(280));
    double hardFrequency = crossingFrequency(hardTuned);
    std::cout << "Hard tune 280 Hz -> " << hardFrequency << " Hz (target D4)\n";
    assert(std::abs(hardFrequency - 293.665) < 6);
    NoteDetector fastDetector; fastDetector.reset(true);
    double fastRatio = 1;
    for (short value : tone(280, 2400)) fastRatio = fastDetector.process(value / 32768.0);
    assert(std::abs(280 * fastRatio - 293.665) < 1);
    NoteDetector noiseDetector;
    unsigned int random = 123;
    double correction = 1;
    for (int i = 0; i < rate; ++i) {
        random = random * 1664525u + 1013904223u;
        correction = noiseDetector.process(((int)(random >> 16) - 32768) / 65536.0);
        if (i > rate / 2) assert(correction == 1);
    }
    options.preset = Deep;
    options.volume = 100; auto loud = processed(options, input);
    options.volume = 50; auto quiet = processed(options, input);
    for (size_t i = 0; i < loud.size(); ++i) assert(std::abs(loud[i] / 2 - quiet[i]) <= 1);

    InitializeCriticalSection(&cs);
    ts3Functions.printMessageToCurrentTab = print;
    ts3Functions.getConnectionStatus = status;
    ts3Functions.getConfigPath = configPath;
    setDefaults();
    assert(!g_voiceAudio.active());
    assert(voiceConnectionReady(1));
    copyOwnership(2, true); assert(!voiceConnectionReady(2)); copyOwnership(2, false);
    assert(g_voiceAudio.start(1, options, g_voiceAudio.generation()));
    short stereo[960];
    for (int i = 0; i < 480; ++i) { stereo[2 * i] = 14000; stereo[2 * i + 1] = 6000; }
    int flags = 2;
    ts3plugin_onEditCapturedVoiceDataEvent(1, stereo, 480, 2, &flags);
    assert(flags == 3);
    for (int i = 0; i < 480; ++i) assert(stereo[i * 2] == stereo[i * 2 + 1]);
    auto unchanged = input;
    flags = 2; ts3plugin_onEditCapturedVoiceDataEvent(2, unchanged.data(), 480, 1, &flags);
    assert(unchanged == input && flags == 2);
    flags = 16; ts3plugin_onEditCapturedVoiceDataEvent(1, unchanged.data(), 480, 1, &flags);
    assert(unchanged == input && flags == 16); // No forced transmission or muted-word buffer.
    std::vector<short> silence(4800);
    flags = 2; ts3plugin_onEditCapturedVoiceDataEvent(1, silence.data(), (int)silence.size(), 1, &flags);
    assert(std::all_of(silence.begin(), silence.end(), [](short v) { return !v; }));
    unchanged = input; flags = 2;
    ts3plugin_onEditCapturedVoiceDataEvent(1, unchanged.data(), 4800, 1, &flags);
    ts3plugin_onClientSelfVariableUpdateEvent(1, CLIENT_INPUT_MUTED, "0", "1");
    flags = 2; ts3plugin_onEditCapturedVoiceDataEvent(1, silence.data(), (int)silence.size(), 1, &flags);
    assert(g_voiceAudio.active() && std::all_of(silence.begin(), silence.end(), [](short v) { return !v; }));
    unchanged = input;
    for (int channels : {0, 9}) {
        flags = 2; ts3plugin_onEditCapturedVoiceDataEvent(1, unchanged.data(), 480, channels, &flags);
        assert(unchanged == input && flags == 2);
    }
    flags = 2;
    ts3plugin_onEditCapturedVoiceDataEvent(1, nullptr, 480, 1, &flags);
    ts3plugin_onEditCapturedVoiceDataEvent(1, unchanged.data(), 480, 1, nullptr);
    ts3plugin_onEditCapturedVoiceDataEvent(1, unchanged.data(), rate + 1, 1, &flags);
    assert(unchanged == input && flags == 2);

    // Talk Back has exclusive capture priority even if armed outside the UI.
    TalkBack::Options talkOptions;
    g_talkAudio.arm(1, 42, talkOptions);
    short masked[480]; std::fill(masked, masked + 480, (short)10000); flags = 2;
    ts3plugin_onEditCapturedVoiceDataEvent(1, masked, 480, 1, &flags);
    assert(!(flags & 2) && std::all_of(masked, masked + 480, [](short v) { return !v; }));
    g_talkAudio.stop();

    uint64_t stale = g_voiceAudio.generation(); hardStop();
    assert(!g_voiceAudio.active() && !g_voiceAudio.start(1, options, stale));
    flags = 2; ts3plugin_onEditCapturedVoiceDataEvent(1, unchanged.data(), 480, 1, &flags);
    assert(unchanged == input && flags == 2);
    assert(g_voiceAudio.start(1, options, g_voiceAudio.generation()));
    stale = g_voiceAudio.generation();
    ts3plugin_onMenuItemEvent(1, PLUGIN_MENU_TYPE_GLOBAL, MENU_VOICE_OFF, 0);
    assert(!g_voiceAudio.active() && !g_voiceAudio.start(1, options, stale));
    assert(g_voiceAudio.start(1, options, g_voiceAudio.generation()));
    ts3plugin_onConnectStatusChangeEvent(2, STATUS_DISCONNECTED, 0);
    assert(g_voiceAudio.active());
    stale = g_voiceAudio.generation();
    ts3plugin_onConnectStatusChangeEvent(1, STATUS_DISCONNECTED, 0);
    assert(!g_voiceAudio.active() && !g_voiceAudio.start(1, options, stale));
    for (int invalid : {-1, (int)Count, 100}) { options.preset = invalid; assert(!g_voiceAudio.start(1, options, g_voiceAudio.generation())); }
    options.preset = Robot; options.volume = 101; assert(!options.valid());
    options.volume = 0; assert(!options.valid()); options.volume = 80;

    // Stop and preset changes can race with capture without corrupting DSP state.
    std::thread capture([&] {
        for (int i = 0; i < 300; ++i) {
            short samples[480] = {}; int edited = 2;
            g_voiceAudio.captured(1, samples, 480, 1, &edited);
            assert(edited & 2);
        }
    });
    for (int i = 0; i < 100; ++i) {
        options.preset = i % Count;
        g_voiceAudio.start(1, options, g_voiceAudio.generation()); g_voiceAudio.stop();
    }
    capture.join();

    char temporary[MAX_PATH]; GetTempPathA(sizeof(temporary), temporary);
    config = std::string(temporary) + "funplugin-voice-" + std::to_string(GetCurrentProcessId()) + "\\";
    assert(CreateDirectoryA(config.c_str(), NULL));
    g.voiceChanger.preset = Alien; g.voiceChanger.volume = 63; saveSettings(); loadSettings();
    assert(g.voiceChanger.preset == Alien && g.voiceChanger.volume == 63 && !g_voiceAudio.active());
    WritePrivateProfileStringA("voicechanger", "preset", "999", (config + "funplugin.ini").c_str());
    loadSettings(); assert(g.voiceChanger.preset == Robot && g.voiceChanger.volume == 80);
    assert(DeleteFileA((config + "funplugin.ini").c_str())); assert(RemoveDirectoryA(config.c_str()));

    hInstance = GetModuleHandleW(nullptr);
    VoiceDialog data;
    HWND dialog = CreateDialogParamW(hInstance, MAKEINTRESOURCEW(IDD_VOICE_CHANGER), nullptr, voiceChangerProc, (LPARAM)&data);
    assert(dialog);
    assert(SendDlgItemMessageW(dialog, IDC_VC_PRESET, CB_GETCOUNT, 0, 0) == Count);
    assert(GetDlgItemInt(dialog, IDC_VC_VOLUME, nullptr, FALSE) == 80);
    SendDlgItemMessageW(dialog, IDC_VC_PRESET, CB_SETCURSEL, AutoTune, 0);
    SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDC_VC_PRESET, CBN_SELCHANGE), 0);
    wchar_t description[256] = {};
    GetDlgItemTextW(dialog, IDC_VC_DESCRIPTION, description, 256);
    assert(std::wstring(description) == VoiceChanger::description(AutoTune));
    DestroyWindow(dialog);
    assert(g_voiceAudio.start(1, options, g_voiceAudio.generation()));
    ts3plugin_shutdown(); assert(!g_voiceAudio.active());
    assert(ts3plugin_init() == 0 && !g_voiceAudio.active()); ts3plugin_shutdown();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
    std::cout << "PASS: 9 distinct presets, pitch/correction, volume, silence, blocks, stereo, callback flags, routing, mute flush, stops, disconnect, concurrency, persistence, native UI (" << duration << " ms)\n";
}
