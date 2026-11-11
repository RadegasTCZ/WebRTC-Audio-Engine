// SPDX-License-Identifier: GPL-3.0-or-later
// AEC3 tuning presets and the percent -> EchoCanceller3Config mapping, adapted from
// AEC3-Echo-Canceller (Source/Strength.cpp, Source/AecMapping.cpp, EchoCancellerEngine::createAEC3Instance)
// by RadegasTCZ.
#pragma once
#include <algorithm>
#include <array>
#include "api/audio/echo_canceller3_config.h"

namespace aecconfig
{
    // Order: lfSupp, lfTransp, hfSupp, hfTransp, lfAttack, lfDecay, hfDucking, hfEchoThr,
    //        dtLfSupp, dtLfTransp, dtHfSupp, dtHfTransp, dtLfAttack, dtLfDecay   (all percent)
    using Tuning = std::array<float, 14>;
    inline const Tuning kGentle{50, 60, 50, 60, 25, 15, 0, 30, 15, 20, 25, 60, 25, 15};
    inline const Tuning kNormal{75, 80, 80, 85, 35, 25, 0, 55, 25, 30, 40, 80, 35, 25};
    inline const Tuning kStrong{95, 100, 95, 100, 55, 50, 50, 80, 50, 55, 60, 90, 55, 40};

    inline const Tuning& ForStrength(unsigned s) { return s == 0 ? kGentle : s == 2 ? kStrong : kNormal; }

    inline float T(float pct) { return std::clamp(pct, 0.0f, 100.0f) / 100.0f; }
    inline float Supp(float p) { return 1.5f - T(p) * 1.49f; }
    inline float HfSupp(float p) { return 0.5f - T(p) * 0.49f; }
    inline float Transp(float p) { return 1.5f - T(p) * 1.49f; }
    inline float HfTransp(float p) { return 0.5f - T(p) * 0.495f; }
    inline float Attack(float p) { return 0.5f + T(p) * 4.5f; }
    inline float Decay(float p) { return 0.01f + T(p) * 0.99f; }
    inline float HfDucking(float p) { return 1.0f - T(p); }
    inline float HfEchoThreshold(float p) { return 2.0f - T(p) * 1.9f; }

    inline webrtc::EchoCanceller3Config Build(const Tuning& t, int roomReverbBlocks = 13, bool clockDrift = false)
    {
        auto strictLt = [](float& transp, float& supp) {
            if (transp >= supp) transp = supp - 0.001f;  // AEC3 requires enr_transparent < enr_suppress
            if (transp < 0.0f) transp = 0.0f;
        };
        float lfT = Transp(t[1]), lfS = Supp(t[0]), hfT = HfTransp(t[3]), hfS = HfSupp(t[2]);
        float dlfT = Transp(t[9]), dlfS = Supp(t[8]), dhfT = HfTransp(t[11]), dhfS = HfSupp(t[10]);
        strictLt(lfT, lfS); strictLt(hfT, hfS); strictLt(dlfT, dlfS); strictLt(dhfT, dhfS);

        using MT = webrtc::EchoCanceller3Config::Suppressor::MaskingThresholds;
        webrtc::EchoCanceller3Config c;
        c.suppressor.normal_tuning.mask_lf = MT(lfT, lfS, 0.3f);
        c.suppressor.normal_tuning.mask_hf = MT(hfT, hfS, 0.3f);
        c.suppressor.normal_tuning.max_inc_factor = Attack(t[4]);
        c.suppressor.normal_tuning.max_dec_factor_lf = Decay(t[5]);
        c.suppressor.nearend_tuning.mask_lf = MT(dlfT, dlfS, 0.3f);
        c.suppressor.nearend_tuning.mask_hf = MT(dhfT, dhfS, 0.3f);
        c.suppressor.nearend_tuning.max_inc_factor = Attack(t[12]);
        c.suppressor.nearend_tuning.max_dec_factor_lf = Decay(t[13]);
        c.suppressor.high_bands_suppression.max_gain_during_echo = HfDucking(t[6]);
        c.suppressor.high_bands_suppression.enr_threshold = HfEchoThreshold(t[7]);
        const size_t len = static_cast<size_t>(std::clamp(roomReverbBlocks, 13, 40));
        c.filter.refined.length_blocks = len;
        c.filter.coarse.length_blocks = len;
        c.echo_removal_control.has_clock_drift = clockDrift;
        return c;
    }
}
