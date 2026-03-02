/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
