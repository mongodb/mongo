// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/exec/shard_tag.h"

#include <iostream>
#include <string_view>

namespace mongo {

// Initialize static class members.

// The actual string values used here are not significant, as they are only used to distinguish one
// 'ShardTag' instance from another.
const ShardTag ShardTag::kDefault{"default"};
const ShardTag ShardTag::kConfigServer{"config"};
const ShardTag ShardTag::kDataShard{"data"};

std::string_view ShardTag::toString() const {
    return std::string_view{tag};
}

std::ostream& operator<<(std::ostream& os, const ShardTag& shardTag) {
    os << shardTag.tag;
    return os;
};

}  // namespace mongo
