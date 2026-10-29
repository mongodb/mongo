// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/modules.h"

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#pragma once

namespace mongo {

/**
 * Represents a robustness level of the change stream reader with regards to required but removed
 * from the system data shards.
 */
enum class [[MONGO_MOD_PUBLIC]] ChangeStreamReadMode {
    // A removed shard causes the change stream reading process to fail.
    kStrict,

    // A removed shard is ignored and the change stream reading process continues.
    kIgnoreRemovedShards,
};

inline ChangeStreamReadMode fromIgnoreRemovedShardsParameter(const boost::optional<bool>& value) {
    return value.has_value() && *value ? ChangeStreamReadMode::kIgnoreRemovedShards
                                       : ChangeStreamReadMode::kStrict;
}

}  // namespace mongo
