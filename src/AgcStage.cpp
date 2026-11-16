// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RadegasTCZ
// Adapted from AEC3-Echo-Canceller (Source/AgcStage.cpp, Source/OutputLimiter.cpp) by RadegasTCZ.
#include "AgcStage.h"
#include <algorithm>
#include <cmath>
#include "api/audio/audio_processing.h"
#include "api/audio/audio_view.h"
#include "api/environment/environment_factory.h"
#include "common_audio/include/audio_util.h"
#include "modules/audio_processing/agc2/adaptive_digital_gain_controller.h"
#include "modules/audio_processing/agc2/agc2_common.h"
#include "modules/audio_processing/agc2/cpu_features.h"
#include "modules/audio_processing/agc2/fixed_digital_level_estimator.h"
#include "modules/audio_processing/agc2/limiter.h"
#include "modules/audio_processing/agc2/noise_level_estimator.h"
#include "modules/audio_processing/agc2/saturation_protector.h"
#include "modules/audio_processing/agc2/speech_level_estimator.h"
#include "modules/audio_processing/agc2/vad_wrapper.h"
#include "modules/audio_processing/logging/apm_data_dumper.h"

namespace
{
    constexpr float kFullScale = 32768.0f;
    float Fix(float v, float def, float lo, float hi) { return std::isfinite(v) ? std::clamp(v, lo, hi) : def; }
}

AgcParams AgcParams::Sanitised() const noexcept
{
    const AgcParams d;
    AgcParams s;
    s.headroomDb = Fix(headroomDb, d.headroomDb, 0.0f, 20.0f);
    s.maxGainDb = Fix(maxGainDb, d.maxGainDb, 1.0f, 50.0f);
    s.adaptSpeedDbPerSec = Fix(adaptSpeedDbPerSec, d.adaptSpeedDbPerSec, 1.0f, 30.0f);
    s.noiseCeilingDbfs = Fix(noiseCeilingDbfs, d.noiseCeilingDbfs, -80.0f, -20.0f);
    return s;
}

AgcStage::AgcStage(const AgcParams& raw, float initialGainDb)
    : m_params(raw.Sanitised()), m_dumper(std::make_unique<webrtc::ApmDataDumper>(0))
{
    webrtc::AudioProcessing::Config::GainController2::AdaptiveDigital cfg;
    cfg.enabled = true;
    cfg.headroom_db = m_params.headroomDb;
    cfg.max_gain_db = m_params.maxGainDb;
    cfg.initial_gain_db = std::isfinite(initialGainDb) && initialGainDb >= 0.0f
                              ? std::clamp(initialGainDb, 0.0f, m_params.maxGainDb)
                              : std::min(15.0f, m_params.maxGainDb);
    m_lastGainDb = cfg.initial_gain_db;
    cfg.max_gain_change_db_per_second = m_params.adaptSpeedDbPerSec;
    cfg.max_output_noise_level_dbfs = m_params.noiseCeilingDbfs;

    const auto env = webrtc::CreateEnvironment();
    m_speechLevel = webrtc::SpeechLevelEstimator::Create(env.field_trials(), m_dumper.get(), cfg,
                                                         webrtc::kAdjacentSpeechFramesThreshold);
    m_vad = std::make_unique<webrtc::VoiceActivityDetectorWrapper>(webrtc::kVadResetPeriodMs,
                                                                   webrtc::GetAvailableCpuFeatures(), kSampleRate);
    m_noiseLevel = webrtc::CreateNoiseFloorEstimator(m_dumper.get());
    m_saturationProtector = webrtc::CreateSaturationProtector(webrtc::kSaturationProtectorInitialHeadroomDb,
                                                              webrtc::kAdjacentSpeechFramesThreshold, m_dumper.get());
    m_controller = std::make_unique<webrtc::AdaptiveDigitalGainController>(m_dumper.get(), cfg,
                                                                           webrtc::kAdjacentSpeechFramesThreshold);
    m_outputLevel = std::make_unique<webrtc::FixedDigitalLevelEstimator>(kFrameSize, m_dumper.get());
}

AgcStage::~AgcStage() = default;

void AgcStage::Process(float* frame) noexcept
{
    float inSq = 0.0f, peak = 0.0f;
    for (int i = 0; i < kFrameSize; ++i)
    {
        const float x = frame[i] * kFullScale;
        m_buffer[i] = x;
        inSq += x * x;
        peak = std::max(peak, std::abs(x));
    }
    webrtc::DeinterleavedView<float> view(m_buffer, kFrameSize, 1);
    webrtc::DeinterleavedView<const float> constView(m_buffer, kFrameSize, 1);

    const float prob = m_vad->Analyze(constView);
    m_lastSpeechProb = prob;
    const float rmsDbfs = webrtc::FloatS16ToDbfs(std::sqrt(inSq / static_cast<float>(kFrameSize)));
    const float peakDbfs = webrtc::FloatS16ToDbfs(peak);
    const float noiseDbfs = m_noiseLevel->Analyze(constView);

    m_speechLevel->Update(rmsDbfs, prob);
    const float targetProb = m_speechLevel->IsTargetSpeakerActive() ? prob : 0.0f;
    const float speechDbfs = m_speechLevel->GetLevelDbfs();
    const bool confident = m_speechLevel->IsConfident();

    m_saturationProtector->Analyze(targetProb, peakDbfs, speechDbfs);
    const float headroomDb = m_saturationProtector->HeadroomDb();
    const float envelopeDbfs = webrtc::FloatS16ToDbfs(m_outputLevel->LastAudioLevel());

    m_controller->Process({.speech_probability = targetProb,
                           .speech_level_dbfs = speechDbfs,
                           .speech_level_reliable = confident,
                           .noise_rms_dbfs = noiseDbfs,
                           .headroom_db = headroomDb,
                           .limiter_envelope_dbfs = envelopeDbfs},
                          view);
    m_outputLevel->ComputeLevel(constView);

    float outSq = 0.0f;
    for (int i = 0; i < kFrameSize; ++i)
    {
        outSq += m_buffer[i] * m_buffer[i];
        frame[i] = m_buffer[i] / kFullScale;
    }
    if (inSq > 1.0f && outSq > 0.0f)
        m_lastGainDb = 10.0f * std::log10(outSq / inSq);
}

// ------------------------------------------------------------------ limiter

namespace
{
    constexpr float kUp = kFullScale / OutputLimiter::kCeilingLinear;
    constexpr float kDown = OutputLimiter::kCeilingLinear / kFullScale;
}

OutputLimiter::OutputLimiter()
    : m_dumper(std::make_unique<webrtc::ApmDataDumper>(0)),
      m_limiter(std::make_unique<webrtc::Limiter>(m_dumper.get(), kFrameSize, "Agc2"))
{
}
OutputLimiter::~OutputLimiter() = default;

void OutputLimiter::Reset() noexcept
{
    m_limiter->Reset();
    m_lastReductionDb = 0.0f;
}

void OutputLimiter::Process(const float* in, float* out) noexcept
{
    float inPeak = 0.0f;
    for (int i = 0; i < kFrameSize; ++i)
    {
        inPeak = std::max(inPeak, std::abs(in[i]));
        m_buffer[i] = in[i] * kUp;
    }
    m_limiter->Process(webrtc::DeinterleavedView<float>(m_buffer, kFrameSize, 1));
    float outPeak = 0.0f;
    for (int i = 0; i < kFrameSize; ++i)
    {
        out[i] = m_buffer[i] * kDown;
        outPeak = std::max(outPeak, std::abs(out[i]));
    }
    m_lastReductionDb = (inPeak > 0.0f && outPeak > 0.0f) ? std::max(0.0f, 20.0f * std::log10(inPeak / outPeak)) : 0.0f;
}
