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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/s/query/exec/async_results_merger_params_gen.h"
#include "mongo/s/query/exec/blocking_results_merger.h"
#include "mongo/s/query/exec/next_high_watermark_determining_strategy.h"
#include "mongo/s/query/exec/router_stage_merge.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/duration.h"

#include <memory>
#include <set>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

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
        const boost::intrusive_ptr<ExpressionContext>&, AsyncResultsMergerParams);

    /**
     * Extracts the remote cursors and converts the execution machinery from a DocumentSource to a
     * RouterStage interface. Can only be called at planning time before any call to getNext().
     */
    std::unique_ptr<RouterStageMerge> convertToRouterStage();

    const char* getSourceName() const final {
        return kStageName.data();
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    void detachSourceFromOperationContext() final;

    void reattachSourceToOperationContext(OperationContext* opCtx) final;

    /**
     * Serializes this stage to be sent to perform the merging on a different host.
     */
    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kNotAllowed,
                                     UnionRequirement::kNotAllowed);

        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    /**
     * Marks the remote cursors as unowned, meaning that they won't be killed upon disposing of this
     * DocumentSource.
     */
    void dismissCursorOwnership() {
        _ownCursors = false;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    /**
     * Set the initial high watermark to return when no cursors are tracked.
     * */
    void setInitialHighWaterMark(const BSONObj& highWaterMark);

    /**
     * Set the strategy to determine the next high water mark.
     * Assumes that the 'AsyncResultsMerger' is in tailable, awaitData mode.
     */
    void setNextHighWaterMarkDeterminingStrategy(
        NextHighWaterMarkDeterminingStrategyPtr nextHighWaterMarkDeterminer);

    ~DocumentSourceMergeCursors() override;

    /**
     * Converts '_armParams' into the execution machinery to merge the cursors. See below for why
     * this is done lazily. Clears '_armParams' and populates '_blockingResultsMerger'. This method
     * cannot be called repeatedly on the same object instance.
     */
    std::shared_ptr<BlockingResultsMerger>& populateMerger();

private:
    friend class DocumentSourceMergeCursorsMultiTenancyTest;

    DocumentSourceMergeCursors(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               AsyncResultsMergerParams armParams);

    /**
     * Get the Async Results Merger Params or return boost:none if it is not set. This is used by
     * DocumentSourceMergeCursorsMultiTenancyTest for unit test purpose.
     */
    boost::optional<NamespaceString> getAsyncResultMergerParamsNss_forTest() const;

    // '_blockingResultsMerger' is lazily populated. Until we need to use it, '_armParams' will be
    // populated with the parameters. Once we start using '_blockingResultsMerger', '_armParams'
    // will become boost::none. We do this to prevent populating '_blockingResultsMerger' on mongos
    // before serializing this stage and sending it to a shard to perform the merge. If we always
    // populated '_blockingResultsMerger', then the destruction of this stage would cause the
    // cursors within '_blockingResultsMerger' to be killed prematurely. For example, if this stage
    // is parsed on mongos then forwarded to the shards, it should not kill the cursors when it goes
    // out of scope on mongos.
    // Note that there is a single case in which neither _armParams nor _blockingResultsMerger are
    // set, and this after convertToRouterStage() is called. After that call the DocumentSource will
    // remain in an unusable state.
    boost::optional<AsyncResultsMergerParams> _armParams;

    // Can only be populated if _armParams is not set. Not populated initially.
    std::shared_ptr<BlockingResultsMerger> _blockingResultsMerger;

    // Indicates whether the cursors stored in _armParams are "owned", meaning the cursors should be
    // killed upon disposal of this DocumentSource.
    bool _ownCursors = true;
};

}  // namespace mongo
