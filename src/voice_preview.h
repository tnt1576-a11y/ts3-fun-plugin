#ifndef FUNPLUGIN_VOICE_PREVIEW_H
#define FUNPLUGIN_VOICE_PREVIEW_H
#include <mmsystem.h>
#include <atomic>
#include <vector>
#include "voice_changer.h"

// Device operations run on the dialog thread. The TS capture callback only
// reads the atomic local-test flag. Neither recording nor playback reaches TS.
class VoicePreview {
    HWAVEIN input = nullptr;
    HWAVEOUT output = nullptr;
    WAVEHDR inHeader{}, outHeader{};
    std::vector<short> raw, rendered;
    bool inputPrepared = false, outputPrepared = false;
    std::atomic<bool> localTest{false};
    WAVEFORMATEX format() const {
        WAVEFORMATEX f{}; f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 1;
        f.nSamplesPerSec = VoiceChanger::rate; f.wBitsPerSample = 16;
        f.nBlockAlign = 2; f.nAvgBytesPerSec = f.nSamplesPerSec * 2; return f;
    }
    void closeInput() {
        if (!input) return;
        waveInReset(input);
        if (inputPrepared) waveInUnprepareHeader(input, &inHeader, sizeof(inHeader));
        waveInClose(input); input = nullptr; inputPrepared = false;
    }
    void closeOutput() {
        if (!output) return;
        waveOutReset(output);
        if (outputPrepared) waveOutUnprepareHeader(output, &outHeader, sizeof(outHeader));
        waveOutClose(output); output = nullptr; outputPrepared = false;
    }
public:
    enum State { Empty, Recording, Ready, Playing };
    State state = Empty;
    bool suppressTransmission() const { return localTest.load(); }
    void stop(bool discard = true) {
        closeInput(); closeOutput(); localTest.store(false);
        if (discard) { raw.clear(); rendered.clear(); }
        state = raw.empty() ? Empty : Ready;
    }
    bool record() {
        stop(); raw.assign(VoiceChanger::rate * 5, 0); inHeader = WAVEHDR{};
        inHeader.lpData = (LPSTR)raw.data(); inHeader.dwBufferLength = (DWORD)(raw.size() * sizeof(short));
        auto f = format(); localTest.store(true);
        if (waveInOpen(&input, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { stop(); return false; }
        inputPrepared = waveInPrepareHeader(input, &inHeader, sizeof(inHeader)) == MMSYSERR_NOERROR;
        if (!inputPrepared || waveInAddBuffer(input, &inHeader, sizeof(inHeader)) != MMSYSERR_NOERROR ||
            waveInStart(input) != MMSYSERR_NOERROR) { stop(); return false; }
        state = Recording; return true;
    }
    bool play(VoiceChanger::Options options) {
        if (state != Ready || raw.empty() || !options.valid()) return false;
        rendered.resize(raw.size() + 4096);
        VoiceChanger::Processor processor; processor.reset(options);
        for (size_t i = 0; i < rendered.size(); ++i) rendered[i] = processor.process(i < raw.size() ? raw[i] : 0);
        outHeader = WAVEHDR{}; outHeader.lpData = (LPSTR)rendered.data();
        outHeader.dwBufferLength = (DWORD)(rendered.size() * sizeof(short));
        auto f = format(); localTest.store(true);
        if (waveOutOpen(&output, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { stop(false); return false; }
        outputPrepared = waveOutPrepareHeader(output, &outHeader, sizeof(outHeader)) == MMSYSERR_NOERROR;
        if (!outputPrepared || waveOutWrite(output, &outHeader, sizeof(outHeader)) != MMSYSERR_NOERROR) { stop(false); return false; }
        state = Playing; return true;
    }
    void tick() {
        if (state == Recording && (inHeader.dwFlags & WHDR_DONE)) {
            size_t count = inHeader.dwBytesRecorded / sizeof(short);
            closeInput(); raw.resize((std::min)(count, raw.size()));
            localTest.store(false); state = raw.empty() ? Empty : Ready;
        }
        if (state == Playing && (outHeader.dwFlags & WHDR_DONE)) {
            closeOutput(); localTest.store(false); state = Ready;
        }
    }
};
#endif
