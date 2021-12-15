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
     * Checks if the collection should be running defragmentation. If a new defragmentation should
     * be started, this will initialize the defragmentation. If defragmentation has been turned off
     * on a collection, this will stop the defragmentation.
     */
    virtual void refreshCollectionDefragmentationStatus(OperationContext* opCtx,
                                                        const CollectionType& coll) = 0;

    /**
     * Returns true if the specified collection is currently being defragmented.
     */
    virtual bool isDefragmentingCollection(const UUID& uuid) = 0;

    /**
     * Generates a descriptor detailing the next defragmentation action (and the targeted
     * collection/chunk[s]) to be performed.
     *
     * The balancer is expected to execute a command matching the content of the descriptor and to
     * invoke the related acknowledge() method on the defragmentation policy once the result is
     * available (this will allow to update the progress of the algorithm).
     *
     * This call blocks when there is no action to be performed (no collection to be defragmented),
     * or when there are too many outstanding actions (too many calls to getStreamingAction() that
     * have not been acknowledged).
     */
    virtual SemiFuture<DefragmentationAction> getNextStreamingAction(OperationContext* opCtx) = 0;

    /**
     * Stops the generation of new actions: any new call to (or currently blocked ones on)
     * getNextStreamingAction() will receive an empty descriptor. Meant to be invoked as part of the
     * balancer shutdown sequence.
     */
    virtual void closeActionStream() = 0;

    virtual void acknowledgeMergeResult(OperationContext* opCtx,
                                        MergeInfo action,
                                        const Status& result) = 0;

    virtual void acknowledgeAutoSplitVectorResult(OperationContext* opCtx,
                                                  AutoSplitVectorInfo action,
                                                  const StatusWith<SplitPoints>& result) = 0;

    virtual void acknowledgeSplitResult(OperationContext* opCtx,
                                        SplitInfoWithKeyPattern action,
                                        const Status& result) = 0;

    virtual void acknowledgeDataSizeResult(OperationContext* opCtx,
                                           DataSizeInfo action,
                                           const StatusWith<DataSizeResponse>& result) = 0;
};
}  // namespace mongo
