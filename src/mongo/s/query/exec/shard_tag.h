/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/util/modules.h"

#include <compare>
#include <iosfwd>
#include <string>

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
    StringData toString() const;

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
