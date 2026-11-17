#include <windows.h>
#include <mmsystem.h>
#include <cassert>
#include <iostream>
static WAVEHDR *incoming, *outgoing;
static int step, failAt;
static bool inOpen, inPrepared, outOpen, outPrepared;
static MMRESULT result() { return ++step == failAt ? MMSYSERR_ERROR : MMSYSERR_NOERROR; }
static MMRESULT fake_waveInOpen(HWAVEIN* h, UINT, const WAVEFORMATEX*, DWORD_PTR, DWORD_PTR, DWORD) { auto r=result(); if(!r) { *h=(HWAVEIN)1; inOpen=true; } return r; }
static MMRESULT fake_waveInPrepareHeader(HWAVEIN, WAVEHDR* h, UINT) { auto r=result(); if(!r) { incoming=h; inPrepared=true; } return r; }
static MMRESULT fake_waveInAddBuffer(HWAVEIN, WAVEHDR*, UINT) { return result(); }
static MMRESULT fake_waveInStart(HWAVEIN) { return result(); }
static MMRESULT fake_waveInReset(HWAVEIN) { return 0; }
static MMRESULT fake_waveInUnprepareHeader(HWAVEIN, WAVEHDR*, UINT) { inPrepared=false; return 0; }
static MMRESULT fake_waveInClose(HWAVEIN) { assert(!inPrepared); inOpen=false; return 0; }
static MMRESULT fake_waveOutOpen(HWAVEOUT* h, UINT, const WAVEFORMATEX*, DWORD_PTR, DWORD_PTR, DWORD) { auto r=result(); if(!r) { *h=(HWAVEOUT)1; outOpen=true; } return r; }
static MMRESULT fake_waveOutPrepareHeader(HWAVEOUT, WAVEHDR* h, UINT) { auto r=result(); if(!r) { outgoing=h; outPrepared=true; } return r; }
static MMRESULT fake_waveOutWrite(HWAVEOUT, WAVEHDR*, UINT) { return result(); }
static MMRESULT fake_waveOutReset(HWAVEOUT) { if(outgoing) outgoing->dwFlags|=WHDR_DONE; return 0; }
static MMRESULT fake_waveOutUnprepareHeader(HWAVEOUT, WAVEHDR*, UINT) { assert(outgoing->dwFlags & WHDR_DONE); outPrepared=false; return 0; }
static MMRESULT fake_waveOutClose(HWAVEOUT) { assert(!outPrepared); outOpen=false; return 0; }
#define waveInOpen fake_waveInOpen
#define waveInPrepareHeader fake_waveInPrepareHeader
#define waveInAddBuffer fake_waveInAddBuffer
#define waveInStart fake_waveInStart
#define waveInReset fake_waveInReset
#define waveInUnprepareHeader fake_waveInUnprepareHeader
#define waveInClose fake_waveInClose
#define waveOutOpen fake_waveOutOpen
#define waveOutPrepareHeader fake_waveOutPrepareHeader
#define waveOutWrite fake_waveOutWrite
#define waveOutReset fake_waveOutReset
#define waveOutUnprepareHeader fake_waveOutUnprepareHeader
#define waveOutClose fake_waveOutClose
#include "../src/plugin.cpp"

int main() {
    _set_error_mode(_OUT_TO_STDERR);
    auto& preview = g_voicePreview;
    VoiceChanger::Options options;
    for(int failure=1; failure<=4; ++failure) {
        step=0; failAt=failure; assert(!preview.record());
        assert(!preview.suppressTransmission() && !inOpen && !inPrepared && preview.state==VoicePreview::Empty);
    }
    step=failAt=0; assert(preview.record());
    assert(preview.suppressTransmission());
    short samples[480]={}; int flags=18;
    ts3plugin_onEditCapturedVoiceDataEvent(55,samples,480,1,&flags);
    assert(flags==16);
    for(unsigned i=0;i<incoming->dwBufferLength/2;++i) ((short*)incoming->lpData)[i]=(short)(10000*std::sin(i*0.03));
    incoming->dwBytesRecorded=incoming->dwBufferLength; incoming->dwFlags|=WHDR_DONE;
    preview.tick(); assert(preview.state==VoicePreview::Ready && !preview.suppressTransmission() && !inOpen);
    for(int failure=1; failure<=3; ++failure) {
        step=0; failAt=failure; assert(!preview.play(options));
        assert(!preview.suppressTransmission() && !outOpen && !outPrepared && preview.state==VoicePreview::Ready);
    }
    step=failAt=0;
    assert(preview.play(options)); assert(preview.suppressTransmission());
    assert(outgoing->dwBufferLength > incoming->dwBufferLength);
    outgoing->dwFlags|=WHDR_DONE; preview.tick();
    assert(preview.state==VoicePreview::Ready && !preview.suppressTransmission());
    options.preset=VoiceChanger::AutoTune; assert(preview.play(options));
    preview.stop(); assert(!outOpen && preview.state==VoicePreview::Empty && !preview.suppressTransmission());
    assert(!preview.play(options));
    assert(preview.record()); preview.stop(); assert(!inOpen && !preview.suppressTransmission());
    std::cout << "PASS: preview recording, local-only capture flags, replay, completion, device failures, cancellation and buffer cleanup (mock devices)\n";
}
