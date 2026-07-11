// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/modules.h"


#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/sharding_environment/shard_id.h"

#include <string_view>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {
namespace txn {

using ParticipantsList = std::vector<ShardId>;

enum class PrepareVote {
    kCommit,
    kAbort,
};

std::string_view toString(PrepareVote prepareVote);

using CommitDecision = PrepareVote;

/**
 * String serializer/deserializer for the commit decision enum values.
 */
CommitDecision readCommitDecisionEnumProperty(std::string_view decision);
std::string_view writeCommitDecisionEnumProperty(CommitDecision decision);

}  // namespace txn
}  // namespace mongo
