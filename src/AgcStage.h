// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from AEC3-Echo-Canceller (Source/AgcStage.*, Source/OutputLimiter.*) by RadegasTCZ.
#pragma once
#include <memory>

namespace webrtc
{
class ApmDataDumper;
class VoiceActivityDetectorWrapper;
class SpeechLevelEstimator;
class NoiseLevelEstimator;
class SaturationProtector;
class AdaptiveDigitalGainController;
class FixedDigitalLevelEstimator;
class Limiter;
}

struct AgcParams
{
    float headroomDb = 5.0f;          // 0..20
    float maxGainDb = 30.0f;          // 1..50
    float adaptSpeedDbPerSec = 6.0f;  // 1..30
    float noiseCeilingDbfs = -50.0f;  // -80..-20
    bool operator==(const AgcParams&) const = default;
    AgcParams Sanitised() const noexcept;
};

// WebRTC AGC2 adaptive digital gain (RNN VAD, speech level + noise floor estimators, saturation
// protector). Mono, 480-sample frames at 48 kHz, +-1.0 scale, in place.
class AgcStage
{
public:
    static constexpr int kFrameSize = 480;
    static constexpr int kSampleRate = 48000;

    // Allocates (never call on the real-time thread). initialGainDb < 0: start at min(15, maxGain) dB;
    // otherwise start from that gain (used when parameters change, so the level is not re-learnt).
    explicit AgcStage(const AgcParams& params, float initialGainDb = -1.0f);
    ~AgcStage();

    void Process(float* frame) noexcept;  // never allocates
    float LastGainDb() const noexcept { return m_lastGainDb; }
    float LastSpeechProbability() const noexcept { return m_lastSpeechProb; }  // RNN VAD, 0..1, last frame
    const AgcParams& Params() const noexcept { return m_params; }

private:
    AgcParams m_params;
    std::unique_ptr<webrtc::ApmDataDumper> m_dumper;
    std::unique_ptr<webrtc::VoiceActivityDetectorWrapper> m_vad;
    std::unique_ptr<webrtc::SpeechLevelEstimator> m_speechLevel;
    std::unique_ptr<webrtc::NoiseLevelEstimator> m_noiseLevel;
    std::unique_ptr<webrtc::SaturationProtector> m_saturationProtector;
    std::unique_ptr<webrtc::AdaptiveDigitalGainController> m_controller;
    std::unique_ptr<webrtc::FixedDigitalLevelEstimator> m_outputLevel;
    float m_lastGainDb = 0.0f;
    float m_lastSpeechProb = 0.0f;
    float m_buffer[kFrameSize]{};
};

// WebRTC AGC2 limiter with a -1 dBFS ceiling (instant attack, no added latency). Mono, 480 samples.
class OutputLimiter
{
public:
    static constexpr int kFrameSize = 480;
    static constexpr float kCeilingLinear = 0.8912509f;  // 10^(-1/20)

    OutputLimiter();  // allocates
    ~OutputLimiter();
    void Process(const float* in, float* out) noexcept;  // may alias; never allocates
    void Reset() noexcept;
    float LastReductionDb() const noexcept { return m_lastReductionDb; }

private:
    std::unique_ptr<webrtc::ApmDataDumper> m_dumper;
    std::unique_ptr<webrtc::Limiter> m_limiter;
    float m_lastReductionDb = 0.0f;
    float m_buffer[kFrameSize]{};
};
