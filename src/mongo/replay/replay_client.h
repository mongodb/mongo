// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/replay/replay_config.h"
#include "mongo/util/modules.h"

namespace mongo {
class ServiceContext;
class [[MONGO_MOD_PUBLIC]] ReplayClient {
public:
    void replayRecording(const ReplayConfigs&);
    void replayRecording(const std::string& recordingFileName, const std::string& uri);
};
}  // namespace mongo
