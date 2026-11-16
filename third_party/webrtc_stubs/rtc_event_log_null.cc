// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RadegasTCZ
// Written for this project; it only fills in declarations of WebRTC's own (BSD-3-Clause) API.
// Provides the out-of-line method for RtcEventLogNull.
// The header declares StartLogging() but doesn't define it inline.

#include "api/rtc_event_log/rtc_event_log.h"

namespace webrtc {

bool RtcEventLogNull::StartLogging(
    std::unique_ptr<RtcEventLogOutput> /* output */,
    int64_t /* output_period_ms */) {
  return false;
}

}  // namespace webrtc
