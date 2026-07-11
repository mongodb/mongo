// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/session/logical_session_cache.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

enum class [[MONGO_MOD_PUBLIC]] LogicalSessionCacheServer {
    kSharded,
    kConfigServer,
    kReplicaSet,
    kStandalone
};

[[MONGO_MOD_PUBLIC]] std::unique_ptr<LogicalSessionCache> makeLogicalSessionCacheD(
    LogicalSessionCacheServer state);

}  // namespace mongo
