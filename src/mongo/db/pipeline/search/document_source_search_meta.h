// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/pipeline/search/lite_parsed_search.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

DEFINE_LITE_PARSED_SEARCH_STAGE_DERIVED(SearchMeta);

/**
 * The $searchMeta stage is similar to the $_internalMongotRemote stage except that it consumes
 * metadata cursors.
 */
class DocumentSourceSearchMeta final : public DocumentSourceInternalSearchMongotRemote {
public:
    static constexpr std::string_view kStageName = "$searchMeta"sv;

    static std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    // Same construction API as the parent class.
    using DocumentSourceInternalSearchMongotRemote::DocumentSourceInternalSearchMongotRemote;

    std::string_view getSourceName() const override {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    /**
     * This is the first stage in the pipeline, but we need to gather responses from all shards in
     * order to set $$SEARCH_META appropriately.
     */
    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final;

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        // We must override DocumentSourceInternalSearchMongotRemote::getDependencies() since this
        // stage does not populate any metadata fields.

        // TODO SERVER-101100 Implement logic for dependency analysis.
        return DepsTracker::State::NOT_SUPPORTED;
    }

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const override {
        auto expCtx = newExpCtx ? newExpCtx : getExpCtx();
        auto executor = uassertStatusOK(
            executor::getMongotTaskExecutor(expCtx->getOperationContext()->getServiceContext()));
        auto spec = getMongotRemoteSpec();
        tassert(9497007,
                "Cannot clone with an initialized cursor - it cannot be shared",
                !_sharedState->_cursor);
        if (_mergingPipeline) {
            spec.setMergingPipeline(_mergingPipeline->serializeToBson());
        }

        return make_intrusive<DocumentSourceSearchMeta>(std::move(spec), expCtx, executor);
    }

    size_t getRemoteCursorId() {
        return _remoteCursorId;
    }

    void setRemoteCursorVars(boost::optional<BSONObj> remoteCursorVars) {
        if (remoteCursorVars) {
            _remoteCursorVars = remoteCursorVars->getOwned();
        }
    }

    boost::optional<BSONObj> getRemoteCursorVars() const {
        return _remoteCursorVars;
    }

    std::unique_ptr<executor::TaskExecutorCursor> getCursor() {
        return std::move(_sharedState->_cursor);
    }


private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceSearchMetaToStageFn(
        const boost::intrusive_ptr<DocumentSource>&);

    // An unique id of search stage in the pipeline, currently it is hard coded to 0 because we can
    // only have one search stage and sub-pipelines are not in the same PlanExecutor.
    // We should assign unique ids when we have everything in a single PlanExecutorSBE.
    // TODO SERVER-109825: This should be moved to SearchMetaStage class.
    size_t _remoteCursorId{0};

    boost::optional<BSONObj> _remoteCursorVars;
};

}  // namespace mongo
