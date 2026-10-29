// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <iosfwd>
#include <string>
#include <string_view>

namespace mongo {

/**
 * A ShardTag is a signifier that can be used to distinguish the different roles that a shard can
 * have.
 * The same shard can be used in its role as the config server (embedded config shard) as well as a
 * data shard. For example, in sharded cluster change streams, cursors are opened on the config
 * server and the data shards. In the cluster uses the embedded config shard mode, then two cursors
 * will be opened on the shard that hosts the config server. This is exactly where a 'ShardTag'
 * helps: the 'ShardTag' can be used in calls to open/close cursors, so even if multiple cursors are
 * opened to the same shard, they can be distinguished by their different 'ShardTag' values.
 */
struct ShardTag {
    // Add comparison functionality for the struct.
    auto operator<=>(const ShardTag& other) const = default;

    // Required to make 'ShardTag' attributes usable in our logging system.
    std::string_view toString() const;

    // The actual shard tag string. Can be any value, but different 'ShardTag' instances should use
    // different tag values.
    const std::string tag;

    // This tag is used by default, when no distinction should be made between the different roles
    // of a shard.
    static const ShardTag kDefault;

    // This tag is used to specifically address the config server role of a shard.
    static const ShardTag kConfigServer;

    // This tag is used to specifically address the data shard role of a shard.
    static const ShardTag kDataShard;
};

/**
 * Insertion operator for 'ShardTag'. Can be used for debugging and logging.
 */
std::ostream& operator<<(std::ostream& os, const ShardTag& shardTag);

}  // namespace mongo
