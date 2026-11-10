// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/ce/persistence/persistent_stats_config_gen.h"
#include "mongo/util/modules.h"

namespace mongo {

[[MONGO_MOD_PUBLIC]]
Status validatePersistentStatsConfigIsApplicable(OperationContext* opCtx, bool isView);

[[MONGO_MOD_PUBLIC]]
Status validatePersistentStatsConfig(const PersistentStatsConfig& persistentStatsConfig);

[[MONGO_MOD_PUBLIC]]
Status validatePersistentStatsConfigUpdate(const PersistentStatsConfigUpdate& update);

}  // namespace mongo
