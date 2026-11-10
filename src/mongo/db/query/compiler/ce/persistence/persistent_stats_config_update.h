// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/ce/persistence/persistent_stats_config_gen.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo {

// Applies 'update' to 'current': unmentioned sample sizes are preserved, null removes one, and
// having no sample size left unsets the configuration.
[[MONGO_MOD_PUBLIC]]
boost::optional<PersistentStatsConfig> applyPersistentStatsConfigUpdate(
    const boost::optional<PersistentStatsConfig>& current,
    const PersistentStatsConfigUpdate& update);

}  // namespace mongo
