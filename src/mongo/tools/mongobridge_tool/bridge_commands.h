// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <mutex>
#include <string_view>

namespace mongo {

class BSONObj;

struct HostAndPort;
class Status;
template <typename T>
class StatusWith;

struct HostSettings {
    enum class State { kForward, kHangUp, kDiscard };

    State state = State::kForward;
    Milliseconds delay{0};
    double loss = 0.0;
};

using HostSettingsMap = stdx::unordered_map<HostAndPort, HostSettings>;

class BridgeCommand {
public:
    static StatusWith<BridgeCommand*> findCommand(std::string_view cmdName);

    virtual ~BridgeCommand() = 0;

    virtual Status run(const BSONObj& cmdObj,
                       std::mutex* settingsMutex,
                       HostSettingsMap* settings) = 0;
};

}  // namespace mongo
