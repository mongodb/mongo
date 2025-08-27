/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"

namespace mongo {

/**
 * The $searchMeta stage is similar to the $_internalMongotRemote stage except that it consumes
 * metadata cursors.
 */
class DocumentSourceSearchMeta final : public DocumentSourceInternalSearchMongotRemote {
public:
    static constexpr StringData kStageName = "$searchMeta"_sd;

    static std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    // Same construction API as the parent class.
    using DocumentSourceInternalSearchMongotRemote::DocumentSourceInternalSearchMongotRemote;

    const char* getSourceName() const override {
        return kStageName.data();
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    /**
     * This is the first stage in the pipeline, but we need to gather responses from all shards in
     * order to set $$SEARCH_META appropriately.
     */
    boost::optional<DistributedPlanLogic> distributedPlanLogic() final;

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        // We must override DocumentSourceInternalSearchMongotRemote::getDependencies() since this
        // stage does not populate any metadata fields.

        // TODO SERVER-101100 Implement logic for dependency analysis.
        return DepsTracker::State::NOT_SUPPORTED;
    }

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const override {
        auto expCtx = newExpCtx ? newExpCtx : getExpCtx();
        auto executor =
            executor::getMongotTaskExecutor(expCtx->getOperationContext()->getServiceContext());
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
