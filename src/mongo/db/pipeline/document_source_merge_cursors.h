
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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/query/async_results_merger.h"

namespace mongo {

/**
 * A stage used only internally to merge results that are being gathered from remote hosts, possibly
 * including this host.
 *
 * Does not assume ownership of cursors until the first call to getNext(). This is to allow this
 * stage to be used on mongos without actually iterating the cursors. For example, when this stage
 * is parsed on mongos it may later be decided that the merging should happen on one of the shards.
 * Then this stage is forwarded to the merging shard, and it should not kill the cursors when it
 * goes out of scope on mongos.
 */
class DocumentSourceMergeCursors : public DocumentSource {
public:
    static constexpr StringData kStageName = "$mergeCursors"_sd;

    /**
     * Parses a serialized version of this stage.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement, const boost::intrusive_ptr<ExpressionContext>&);

    /**
     * Creates a new DocumentSourceMergeCursors from the given parameters.
     */
    static boost::intrusive_ptr<DocumentSource> create(
        executor::TaskExecutor*,
        AsyncResultsMergerParams,
        const boost::intrusive_ptr<ExpressionContext>&);

    const char* getSourceName() const final {
        return kStageName.rawData();
    }

    /**
     * Absorbs a subsequent $sort if it's merging pre-sorted streams.
     */
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container);
    void detachFromOperationContext() final;
    void reattachToOperationContext(OperationContext*) final;

    /**
     * Serializes this stage to be sent to perform the merging on a different host.
     */
    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kAnyShard,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     // TODO SERVER-33683: Permit $mergeCursors with readConcern
                                     // level "snapshot".
                                     TransactionRequirement::kNotAllowed);

        constraints.requiresInputDocSource = false;
        return constraints;
    }

    GetNextResult getNext() final;

    /**
     * Returns the high water mark sort key for the given cursor, if it exists; otherwise, returns
     * an empty BSONObj. Calling this method causes the underlying ARM to be populated and assumes
     * ownership of the remote cursors.
     */
    BSONObj getHighWaterMark();

protected:
    void doDispose() final;

private:
    DocumentSourceMergeCursors(executor::TaskExecutor*,
                               AsyncResultsMergerParams,
                               const boost::intrusive_ptr<ExpressionContext>&,
                               boost::optional<BSONObj> ownedParamsSpec = boost::none);

    // When we have parsed the params out of a BSONObj, the object needs to stay around while the
    // params are in use. We store them here.
    boost::optional<BSONObj> _armParamsObj;

    executor::TaskExecutor* _executor;

    // '_armParams' is populated until the first call to getNext(). Upon the first call to getNext()
    // '_arm' will be populated using '_armParams', and '_armParams' will become boost::none. So if
    // getNext() is never called we will never populate '_arm'. If we did so the destruction of this
    // stage would cause the cursors within the ARM to be killed prematurely. For example, if this
    // stage is parsed on mongos then forwarded to the shards, it should not kill the cursors when
    // it goes out of scope on mongos.
    boost::optional<AsyncResultsMergerParams> _armParams;
    boost::optional<AsyncResultsMerger> _arm;
};

}  // namespace mongo
