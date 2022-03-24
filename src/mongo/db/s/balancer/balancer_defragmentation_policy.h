/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/s/catalog/type_collection.h"

namespace mongo {
/**
 * Helper class that
 * - stores the progress of the defragmentation algorithm on each collection
 * - generates a single sequence of action descriptors to fairly execute the defragmentation
 * algorithm across collections.
 */
class BalancerDefragmentationPolicy {

public:
    virtual ~BalancerDefragmentationPolicy() {}

    /**
     * Requests the execution of the defragmentation algorithm on the specified collection.
     * Returns true if the request is accepted, false if ignored (meaning, the specified collection
     * is already being processed)
     */
    virtual void startCollectionDefragmentation(OperationContext* opCtx,
                                                const CollectionType& coll) = 0;

    /**
     * Checks if the collection is currently being defragmented, and signals the defragmentation
     * to end if so.
     */
    virtual void abortCollectionDefragmentation(OperationContext* opCtx,
                                                const NamespaceString& nss) = 0;

    /**
     * Returns true if the specified collection is currently being defragmented.
     */
    virtual bool isDefragmentingCollection(const UUID& uuid) = 0;

    virtual BSONObj reportProgressOn(const UUID& uuid) = 0;

    virtual MigrateInfoVector selectChunksToMove(OperationContext* opCtx,
                                                 stdx::unordered_set<ShardId>* usedShards) = 0;

    /**
     * Generates a descriptor detailing the next defragmentation action (and the targeted
     * collection/chunk[s]) to be performed.
     *
     * The balancer is expected to execute a command matching the content of the descriptor and to
     * invoke the related acknowledge() method on the defragmentation policy once the result is
     * available (this will allow to update the progress of the algorithm).
     */
    virtual boost::optional<DefragmentationAction> getNextStreamingAction(
        OperationContext* opCtx) = 0;

    /**
     * Updates the internal status of the policy by notifying the result of an action previously
     * retrieved through getNextStreamingAction() or selectChunksToMove().
     * The types of action and response are expected to match - or an stdx::bad_variant_access
     * error will be thrown.
     */
    virtual void applyActionResult(OperationContext* opCtx,
                                   const DefragmentationAction& action,
                                   const DefragmentationActionResponse& response) = 0;
};
}  // namespace mongo
