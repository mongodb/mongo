// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/post_resharding_placement.h"
#include "mongo/db/sharding_environment/shard_id.h"
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

/**
 * Computes the destined recipients for the old and new documents using the given
 * PostReshardingCollectionPlacement from a collection acquisition. Returns boost::none
 * if 'reshardingPlacement' is empty or the resharding keys for oldDoc and newDoc are
 * identical. This overload supports the legacy (non resharding registry) code path.
 */
boost::optional<DestinedRecipients> getDestinedRecipientsIfPossiblyDifferent(
    OperationContext* opCtx,
    const boost::optional<PostReshardingCollectionPlacement>& reshardingPlacement,
    const BSONObj& oldDoc,
    const BSONObj& newDoc);

}  // namespace mongo::resharding
