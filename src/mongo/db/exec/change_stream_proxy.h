/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/exec/pipeline_proxy.h"

namespace mongo {

/**
 * ChangeStreamProxyStage is a drop-in replacement for PipelineProxyStage, intended to manage the
 * serialization of change stream pipeline output from Document to BSON. In particular, it is
 * additionally responsible for tracking the latestOplogTimestamps and postBatchResumeTokens that
 * are necessary for correct merging on mongoS and, in the latter case, must also be provided to
 * mongoD clients.
 */
class ChangeStreamProxyStage final : public PipelineProxyStage {
public:
    static const char* kStageType;

    /**
     * The 'pipeline' argument must be a $changeStream pipeline. Passing a non-$changeStream into
     * the constructor will cause an invariant() to fail.
     */
    ChangeStreamProxyStage(OperationContext* opCtx,
                           std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                           WorkingSet* ws);

    /**
     * Returns an empty PlanStageStats object.
     */
    std::unique_ptr<PlanStageStats> getStats() final;

    /**
     * Passes through the latest oplog timestamp from the proxied pipeline. We only expose the oplog
     * timestamp in the event that we need to merge on mongoS.
     */
    Timestamp getLatestOplogTimestamp() const {
        return _includeMetaData ? _latestOplogTimestamp : Timestamp();
    }

    /**
     * Passes through the most recent resume token from the proxied pipeline.
     */
    BSONObj getPostBatchResumeToken() const {
        return _postBatchResumeToken;
    }

    StageType stageType() const final {
        return STAGE_CHANGE_STREAM_PROXY;
    }

protected:
    boost::optional<Document> getNext() final;

private:
    /**
     * Verifies that the docs's resume token has not been modified.
     */
    void _validateResumeToken(const Document& event) const;

    /**
     * Set the speculative majority read timestamp if we have scanned up to a certain oplog
     * timestamp.
     */
    void _setSpeculativeReadTimestamp();

    Timestamp _latestOplogTimestamp;
    BSONObj _postBatchResumeToken;
};
}  // namespace mongo
