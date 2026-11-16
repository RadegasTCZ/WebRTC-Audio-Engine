// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RadegasTCZ
// Written for this project; it only fills in declarations of WebRTC's own (BSD-3-Clause) API.
// Stub: provides CreateDefaultTaskQueueFactory() without pulling in
// platform-specific task queue implementations.
// AEC3 never uses task_queue_factory at runtime, but the Environment
// constructor requires a non-null pointer.

#include "api/task_queue/default_task_queue_factory.h"
#include "api/task_queue/task_queue_factory.h"

#include <memory>

namespace webrtc {
namespace {

class StubTaskQueueFactory final : public TaskQueueFactory {
 public:
  std::unique_ptr<TaskQueueBase, TaskQueueDeleter> CreateTaskQueue(
      absl::string_view /* name */,
      Priority /* priority */) const override {
    // AEC3 never calls this — it only passes Environment through.
    // If this is ever reached, something unexpected is happening.
    std::abort();
  }
};

}  // namespace

std::unique_ptr<TaskQueueFactory> CreateDefaultTaskQueueFactory(
    const FieldTrialsView* /* field_trials */) {
  return std::make_unique<StubTaskQueueFactory>();
}

}  // namespace webrtc
