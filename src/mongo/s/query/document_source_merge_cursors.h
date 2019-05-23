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
#include "mongo/s/query/blocking_results_merger.h"
#include "mongo/s/query/router_stage_merge.h"

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
    static boost::intrusive_ptr<DocumentSourceMergeCursors> create(
        executor::TaskExecutor*,
        AsyncResultsMergerParams,
        const boost::intrusive_ptr<ExpressionContext>&);

    /**
     * Extracts the remote cursors and converts the execution machinery from a DocumentSource to a
     * RouterStage interface. Can only be called at planning time before any call to getNext().
     */
    std::unique_ptr<RouterStageMerge> convertToRouterStage();

    const char* getSourceName() const final {
        return kStageName.rawData();
    }

    void detachFromOperationContext() final;
    void reattachToOperationContext(OperationContext*) final;

    /**
     * Serializes this stage to be sent to perform the merging on a different host.
     */
    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kNotAllowed);

        constraints.requiresInputDocSource = false;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    GetNextResult getNext() final;

    std::size_t getNumRemotes() const;

    /**
     * Returns the high water mark sort key for the given cursor, if it exists; otherwise, returns
     * an empty BSONObj. Calling this method causes the underlying BlockingResultsMerger to be
     * populated and assumes ownership of the remote cursors.
     */
    BSONObj getHighWaterMark();

    bool remotesExhausted() const;

    void setExecContext(RouterExecStage::ExecContext execContext) {
        _execContext = execContext;
    }

    Status setAwaitDataTimeout(Milliseconds awaitDataTimeout) {
        if (!_blockingResultsMerger) {
            // In cases where a cursor was established with a batchSize of 0, the first getMore
            // might specify a custom maxTimeMS (AKA await data timeout). In these cases we will not
            // have iterated the cursor yet so will not have populated the merger, but need to
            // remember/track the custom await data timeout. We will soon iterate the cursor, so we
            // just populate the merger now and let it track the await data timeout itself.
            populateMerger();
        }
        return _blockingResultsMerger->setAwaitDataTimeout(awaitDataTimeout);
    }

    /**
     * Adds the specified shard cursors to the set of cursors to be merged. The results from the
     * new cursors will be returned as normal through getNext().
     */
    void addNewShardCursors(std::vector<RemoteCursor>&& newCursors) {
        invariant(_blockingResultsMerger);
        _blockingResultsMerger->addNewShardCursors(std::move(newCursors));
    }

    /**
     * Marks the remote cursors as unowned, meaning that they won't be killed upon disposing of this
     * DocumentSource.
     */
    void dismissCursorOwnership() {
        _ownCursors = false;
    }

protected:
    void doDispose() final;

private:
    DocumentSourceMergeCursors(executor::TaskExecutor*,
                               AsyncResultsMergerParams,
                               const boost::intrusive_ptr<ExpressionContext>&,
                               boost::optional<BSONObj> ownedParamsSpec = boost::none);

    /**
     * Converts '_armParams' into the execution machinery to merge the cursors. See below for why
     * this is done lazily. Clears '_armParams' and populates '_blockingResultsMerger'.
     */
    void populateMerger();

    // When we have parsed the params out of a BSONObj, the object needs to stay around while the
    // params are in use. We store them here.
    boost::optional<BSONObj> _armParamsObj;

    executor::TaskExecutor* _executor;

    // '_blockingResultsMerger' is lazily populated. Until we need to use it, '_armParams' will be
    // populated with the parameters. Once we start using '_blockingResultsMerger', '_armParams'
    // will become boost::none. We do this to prevent populating '_blockingResultsMerger' on mongos
    // before serializing this stage and sending it to a shard to perform the merge. If we always
    // populated '_blockingResultsMerger', then the destruction of this stage would cause the
    // cursors within '_blockingResultsMerger' to be killed prematurely. For example, if this stage
    // is parsed on mongos then forwarded to the shards, it should not kill the cursors when it goes
    // out of scope on mongos.
    boost::optional<AsyncResultsMergerParams> _armParams;
    boost::optional<BlockingResultsMerger> _blockingResultsMerger;

    // The ExecContext is needed because if we're a tailable, awaitData cursor, we only want to
    // 'await data' if we 1) are in a getMore and 2) don't already have data to return. This context
    // allows us to determine which situation we're in.
    RouterExecStage::ExecContext _execContext = RouterExecStage::ExecContext::kInitialFind;

    // Indicates whether the cursors stored in _armParams are "owned", meaning the cursors should be
    // killed upon disposal of this DocumentSource.
    bool _ownCursors = true;
};

}  // namespace mongo
