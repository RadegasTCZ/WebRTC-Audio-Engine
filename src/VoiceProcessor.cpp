#include "VoiceProcessor.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include "modules/audio_processing/audio_buffer.h"  // third_party/webrtc/shim: minimal stand-in
#include "modules/audio_processing/ns/noise_suppressor.h"
#include "modules/audio_processing/three_band_filter_bank.h"

static_assert(webrtc::ThreeBandFilterBank::kFullBandSize == VoiceProcessor::kBlock, "one block = one filter-bank frame");
static_assert(webrtc::ThreeBandFilterBank::kSplitBandSize == 160 && webrtc::kNsFrameSize == 160, "16 kHz bands of 10 ms");

namespace
{
    constexpr int kBands = webrtc::ThreeBandFilterBank::kNumBands;
    constexpr int kBandSize = webrtc::ThreeBandFilterBank::kSplitBandSize;
}

struct VoiceProcessor::Chain
{
    explicit Chain(Level level)
    {
        webrtc::NsConfig config;
        config.target_level = static_cast<webrtc::NsConfig::SuppressionLevel>(static_cast<int>(level));
        ns = std::make_unique<webrtc::NoiseSuppressor>(config, static_cast<size_t>(kSampleRate), static_cast<size_t>(1));
        for (int b = 0; b < kBands; ++b)
        {
            bandPtrs[b] = bands[b].data();
            bandViews[b] = std::span<float>(bands[b]);
        }
    }

    void Reset() noexcept
    {
        bank = webrtc::ThreeBandFilterBank();  // fixed-size arrays: no allocation
        ns->Reset();                           // local patch, no allocation
    }

    // in/out: 480 samples in [-1, 1] float scale. May alias.
    void Run(const float* in, float* out) noexcept
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const float x = in[i];
            scaled[i] = std::isfinite(x) ? x * 32768.0f : 0.0f;  // NS works in the int16 range
        }
        bank.Analysis(scaled, bandViews);
        webrtc::AudioBuffer buffer(bandPtrs.data(), static_cast<size_t>(kBands), static_cast<size_t>(kBandSize));
        ns->Analyze(buffer);
        ns->Process(&buffer);
        bank.Synthesis(bandViews, synthesis);
        for (int i = 0; i < kBlock; ++i)
            out[i] = synthesis[i] * (1.0f / 32768.0f);
    }

    webrtc::ThreeBandFilterBank bank;
    std::unique_ptr<webrtc::NoiseSuppressor> ns;
    std::array<std::array<float, kBandSize>, kBands> bands{};
    std::array<float*, kBands> bandPtrs{};
    std::array<std::span<float>, kBands> bandViews{};
    std::array<float, kBlock> scaled{};
    std::array<float, kBlock> synthesis{};
};

VoiceProcessor::VoiceProcessor() = default;
VoiceProcessor::~VoiceProcessor() = default;

bool VoiceProcessor::Prepare(int channels, int sampleRate, Level level)
{
    m_active = false;
    m_chains.clear();
    if (sampleRate != kSampleRate || channels < 1 || channels > kMaxChannels)
        return false;

    m_channels = channels;
    m_chains.reserve(static_cast<size_t>(channels));
    for (int c = 0; c < channels; ++c)
        m_chains.push_back(std::make_unique<Chain>(level));
    m_inBlock.assign(static_cast<size_t>(channels), {});
    m_outBlock.assign(static_cast<size_t>(channels), {});
    Reset();
    m_active = true;
    return true;
}

void VoiceProcessor::Reset() noexcept
{
    for (auto& c : m_chains)
        c->Reset();
    for (auto& b : m_inBlock) b.fill(0.0f);
    for (auto& b : m_outBlock) b.fill(0.0f);
    m_pos = 0;
}

void VoiceProcessor::ProcessBlock() noexcept
{
    for (int c = 0; c < m_channels; ++c)
        m_chains[static_cast<size_t>(c)]->Run(m_inBlock[static_cast<size_t>(c)].data(), m_outBlock[static_cast<size_t>(c)].data());
}

void VoiceProcessor::Process(const float* in, float* out, int frames) noexcept
{
    if (!m_active)
    {
        if (in != out)
            std::memcpy(out, in, static_cast<size_t>(frames) * static_cast<size_t>(m_channels > 0 ? m_channels : 1) * sizeof(float));
        return;
    }
    const int ch = m_channels;
    for (int f = 0; f < frames; ++f)
    {
        const size_t base = static_cast<size_t>(f) * static_cast<size_t>(ch);
        for (int c = 0; c < ch; ++c)
        {
            const float x = in[base + c];  // read before write: safe in-place
            m_inBlock[static_cast<size_t>(c)][static_cast<size_t>(m_pos)] = x;
            out[base + c] = m_outBlock[static_cast<size_t>(c)][static_cast<size_t>(m_pos)];
        }
        if (++m_pos == kBlock)
        {
            ProcessBlock();
            m_pos = 0;
        }
    }
}
