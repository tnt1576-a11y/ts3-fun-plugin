#ifndef FUNPLUGIN_VOICE_CHANGER_H
#define FUNPLUGIN_VOICE_CHANGER_H
#include <windows.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace VoiceChanger {
static const int rate = 48000;
static const double pi = 3.14159265358979323846;
enum Preset { AutoTune, Robot, Female, Deep, Baby, Chipmunk, Demon, Alien, Radio, Count };
inline const wchar_t* name(int preset) {
    static const wchar_t* names[] = {L"Auto-tune (hard tune)", L"Robot", L"Female (bright pitch)",
        L"Deep", L"Baby", L"Chipmunk", L"Demon", L"Alien", L"Radio"};
    return preset >= 0 && preset < Count ? names[preset] : L"Unknown";
}
inline const wchar_t* description(int preset) {
    static const wchar_t* descriptions[] = {
        L"Fast note snapping to C major pentatonic for a stronger singing effect. Hold a vowel or slide your pitch to hear the steps.",
        L"Metallic ring modulation for a classic sci-fi robot.",
        L"Higher pitch with a brighter tone. A playful effect, not realistic voice conversion.",
        L"Lower pitch for a deep, booming voice.",
        L"High, bright pitch with a little wobble for a cartoon baby.",
        L"An octave up for a squeaky cartoon voice.",
        L"Extra-low pitch with a rough, rumbling edge.",
        L"Raised pitch with warbling sci-fi modulation.",
        L"Narrow-band, lightly distorted walkie-talkie voice."};
    return preset >= 0 && preset < Count ? descriptions[preset] : L"";
}
struct Options {
    int preset = Robot;
    int volume = 80;
    bool valid() const { return preset >= 0 && preset < Count && volume >= 10 && volume <= 100; }
};

// Bounded YIN-style difference detector at 6 kHz. Analyze every 10 ms, only
// for Auto-tune. No allocation, SDK calls, recording, or FFT in the audio path.
class NoteDetector {
    std::array<float, 512> history{};
    int write = 0, filled = 0, decimation = 0, hop = 0;
    double low1 = 0, low2 = 0;
    double correction = 1;
    bool hard = false;
    float ago(int offset) const { return history[(write - 1 - offset + 1024) % 512]; }
    void analyze() {
        double energy = 0;
        for (int i = 0; i < 128; ++i) energy += ago(i) * ago(i);
        correction = 1;
        if (energy / 128 < 0.00001) return;
        std::array<double, 102> normalized{};
        double sum = 0;
        normalized[0] = 1;
        for (int lag = 1; lag <= 101; ++lag) {
            double difference = 0;
            for (int i = 0; i < 128; ++i) {
                double delta = ago(i) - ago(i + lag);
                difference += delta * delta;
            }
            sum += difference;
            normalized[lag] = sum > 1e-12 ? difference * lag / sum : 1;
        }
        // First strong valley avoids selecting a subharmonic. Unvoiced input
        // retains its pitch; only a confident 60-500 Hz fundamental is tuned.
        for (int lag = 12; lag <= 100; ++lag) {
            if (normalized[lag] >= 0.16) continue;
            while (lag < 100 && normalized[lag + 1] < normalized[lag]) ++lag;
            double a = normalized[lag - 1], b = normalized[lag], c = normalized[lag + 1];
            double denominator = a - 2 * b + c;
            double offset = std::abs(denominator) > 1e-12 ? 0.5 * (a - c) / denominator : 0;
            offset = (std::max)(-0.5, (std::min)(0.5, offset));
            double frequency = 6000.0 / (lag + offset);
            double note = 69 + 12 * std::log2(frequency / 440.0);
            double target = std::round(note);
            if (hard) {
                double best = 100;
                for (int candidate = (int)std::floor(note) - 3; candidate <= (int)std::ceil(note) + 3; ++candidate) {
                    int pitchClass = (candidate % 12 + 12) % 12;
                    if (pitchClass != 0 && pitchClass != 2 && pitchClass != 4 && pitchClass != 7 && pitchClass != 9) continue;
                    if (std::abs(candidate - note) < best) { best = std::abs(candidate - note); target = candidate; }
                }
            }
            correction = std::pow(2.0, (target - note) / 12.0);
            return;
        }
    }
public:
    void reset(bool hardTune = false) { hard = hardTune; history.fill(0); write = filled = decimation = hop = 0; low1 = low2 = 0; correction = 1; }
    double process(double sample) {
        // Two low-pass stages before decimation keep high harmonics from aliasing.
        low1 += 0.20 * (sample - low1); low2 += 0.20 * (low1 - low2);
        if (++decimation == 8) {
            decimation = 0; history[write] = (float)low2; write = (write + 1) % 512;
            if (filled < 512) ++filled;
            if (++hop == 60) { hop = 0; if (filled >= 230) analyze(); }
        }
        return correction;
    }
};

class Processor {
    std::array<float, 4096> history{};
    NoteDetector detector;
    Options options;
    int write = 0, fade = 0;
    double phase = 0, oscillator = 0, wobble = 0, ratio = 1;
    double low = 0, high = 0, previous = 0;
    double read(double delay) const {
        double position = write - delay;
        if (position < 0) position += history.size();
        int a = (int)position, b = (a + 1) % history.size();
        return history[a] + (history[b] - history[a]) * (position - a);
    }
public:
    void reset(Options selected) {
        options = selected; history.fill(0); detector.reset(true); write = fade = 0;
        phase = oscillator = wobble = low = high = previous = 0;
        int semitones = 0;
        switch (options.preset) {
        case Female: semitones = 5; break;
        case Deep: semitones = -7; break;
        case Baby: semitones = 9; break;
        case Chipmunk: semitones = 12; break;
        case Demon: semitones = -10; break;
        case Alien: semitones = 4; break;
        }
        ratio = std::pow(2.0, semitones / 12.0);
    }
    short process(short input) {
        double sample = input / 32768.0;
        if (options.preset == AutoTune) ratio += 0.004 * (detector.process(sample) - ratio);
        double value = sample;
        if (options.preset != Robot && options.preset != Radio) {
            history[write] = (float)sample;
            double other = phase < 0.5 ? phase + 0.5 : phase - 0.5;
            double weight = 0.5 - 0.5 * std::cos(2 * pi * phase);
            value = read(64 + phase * 2048) * weight + read(64 + other * 2048) * (1 - weight);
            write = (write + 1) % history.size();
            double modulation = options.preset == Baby ? 0.012 * std::sin(wobble) : 0;
            phase += (1 - ratio - modulation) / 2048;
            if (phase < 0) phase += 1;
            if (phase >= 1) phase -= 1;
        }
        double frequency = 70;
        if (options.preset == Demon) frequency = 32;
        if (options.preset == Alien) frequency = 105 + 55 * std::sin(wobble);
        oscillator += 2 * pi * frequency / rate;
        if (oscillator >= 2 * pi) oscillator -= 2 * pi;
        wobble += 2 * pi * 5 / rate;
        if (wobble >= 2 * pi) wobble -= 2 * pi;
        switch (options.preset) {
        case Robot: value *= std::sin(oscillator); break;
        case Demon: value *= 0.65 + 0.35 * std::sin(oscillator); break;
        case Alien: value *= 0.35 + 0.65 * std::sin(oscillator); break;
        case Female: case Baby:
            low += 0.12 * (value - low); value = 0.8 * value + 0.2 * (value - low); break;
        case Radio:
            high = 0.96 * (high + value - previous); previous = value;
            low += 0.30 * (high - low);
            value = std::tanh(2.5 * low) * 0.65; break;
        }
        // 5 ms fade-in after a preset change / new utterance avoids a hard edge.
        if (fade < 240) ++fade;
        value *= options.volume / 100.0 * fade / 240.0;
        return (short)((std::max)(-32768.0, (std::min)(32767.0, value * 32768.0)));
    }
};

class Audio {
    SRWLOCK lock = SRWLOCK_INIT;
    uint64_t source = 0, revision = 0;
    Options options;
    Processor processor;
    bool transmitting = false;
    ULONGLONG lastCapture = 0;
public:
    uint64_t generation() {
        AcquireSRWLockShared(&lock); auto value = revision; ReleaseSRWLockShared(&lock); return value;
    }
    bool active() {
        AcquireSRWLockShared(&lock); bool value = source != 0; ReleaseSRWLockShared(&lock); return value;
    }
    bool start(uint64_t connection, Options selected, uint64_t expected) {
        if (!connection || !selected.valid()) return false;
        AcquireSRWLockExclusive(&lock);
        bool accepted = expected == revision;
        if (accepted) {
            source = connection; options = selected; processor.reset(options);
            transmitting = false; lastCapture = 0; ++revision;
        }
        ReleaseSRWLockExclusive(&lock); return accepted;
    }
    // A stop also invalidates a settings dialog that was already open.
    void stop() {
        AcquireSRWLockExclusive(&lock);
        source = 0; ++revision; transmitting = false; lastCapture = 0; processor.reset(options);
        ReleaseSRWLockExclusive(&lock);
    }
    void disconnected(uint64_t connection) {
        AcquireSRWLockExclusive(&lock);
        ++revision;
        if (source == connection) { source = 0; transmitting = false; processor.reset(options); }
        ReleaseSRWLockExclusive(&lock);
    }
    void clearCapture(uint64_t connection) {
        AcquireSRWLockExclusive(&lock);
        if (source == connection) { transmitting = false; lastCapture = 0; processor.reset(options); }
        ReleaseSRWLockExclusive(&lock);
    }
    void captured(uint64_t connection, short* samples, int frames, int channels, int* edited) {
        if (!samples || !edited || frames <= 0 || frames > rate || channels < 1 || channels > 8) return;
        AcquireSRWLockExclusive(&lock);
        if (source && source == connection) {
            ULONGLONG now = GetTickCount64();
            if (!(*edited & 2)) {
                // Never buffer words said while muted / PTT released, or force transmission.
                if (transmitting) processor.reset(options);
                transmitting = false;
            } else {
                if (!transmitting || now - lastCapture > 250) processor.reset(options);
                transmitting = true;
                for (int i = 0; i < frames; ++i) {
                    int sum = 0;
                    for (int c = 0; c < channels; ++c) sum += samples[i * channels + c];
                    short output = processor.process((short)(sum / channels));
                    for (int c = 0; c < channels; ++c) samples[i * channels + c] = output;
                }
                *edited |= 1; // Preserve TeamSpeak's send decision and all other bits.
            }
            lastCapture = now;
        }
        ReleaseSRWLockExclusive(&lock);
    }
};
}
#endif
