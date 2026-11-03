// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/shard_id.h"

#include <boost/optional/optional.hpp>

namespace mongo::resharding {

/**
 * Holds the destined recipient shards for the pre-image and post-image of a document
 * being updated during a resharding operation. Used to determine whether the update
 * would cause the document to change owning shards under the new resharding key.
 */
struct DestinedRecipients {
    ShardId oldRecipient;
    ShardId newRecipient;
};

/**
 * Computes the destined recipients for the old and new documents under the resharding key
 * using the LocalReshardingOperationsRegistry. Returns boost::none if resharding is not
 * active, the resharding operation has committed, or the old and new documents map to the
 * same recipient.
 */
boost::optional<DestinedRecipients> getDestinedRecipientsIfPossiblyDifferent(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& oldDoc,
    const BSONObj& newDoc);

}  // namespace mongo::resharding
