// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/str.h"

#include <string>

namespace mongo {

enum class ReplicaSetMonitorProtocol { kSdam, kStreamable };
extern ReplicaSetMonitorProtocol gReplicaSetMonitorProtocol;
std::string toString(ReplicaSetMonitorProtocol protocol);

}  // namespace mongo
