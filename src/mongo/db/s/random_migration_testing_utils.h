// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/modules.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace random_migration_testing_utils {

/*
 * Gets the list of shards from the config server and checks whether the current shard is marked as
 * draining. Returns false if this shard is not found in the list of all shards, and throws an error
 * if getting the shard list fails.
 */
bool isCurrentShardDraining(OperationContext* opCtx);

/*
 * Chooses any random value within the min and max bounds if there are any. If there are no
 * documents in the range, attempts to generate a random document in the correct range. Returns
 * boost::none if there are no documents in the range and we are unable to generate a random one.
 */
boost::optional<BSONObj> generateRandomSplitPoint(OperationContext* opCtx,
                                                  const CollectionAcquisition& acquisition,
                                                  const BSONObj& skPattern,
                                                  const BSONObj& min,
                                                  const BSONObj& max);

}  // namespace random_migration_testing_utils

}  // namespace mongo
