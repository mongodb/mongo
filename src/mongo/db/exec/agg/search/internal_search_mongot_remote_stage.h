// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/index/sort_key_generator.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/search/internal_search_mongot_remote_spec_gen.h"
#include "mongo/db/query/search/search_query_view_spec_gen.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_cursor.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

class InternalSearchMongotRemoteStage : public Stage {
public:
    using SearchIdLookupMetrics = DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics;

    InternalSearchMongotRemoteStage(
        std::string_view stageName,
        InternalSearchMongotRemoteSpec spec,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const std::shared_ptr<executor::TaskExecutor>& taskExecutor,
        const std::shared_ptr<SearchIdLookupMetrics>& searchIdLookupMetrics,
        const std::shared_ptr<InternalSearchMongotRemoteSharedState>& sharedState);

    BSONObj getSearchQuery() const {
        return _spec.getMongotQuery().getOwned();
    }

    auto getTaskExecutor() const {
        return _taskExecutor;
    }

    boost::optional<SearchQueryViewSpec> getView() {
        return _spec.getView();
    }

    boost::optional<int> getIntermediateResultsProtocolVersion() {
        // If it turns out that this stage is not running on a sharded collection, we don't want
        // to send the protocol version to mongot. If the protocol version is sent, mongot will
        // generate unmerged metadata documents that we won't be set up to merge.
        if (!pExpCtx->getNeedsMerge()) {
            return boost::none;
        }
        return _spec.getMetadataMergeProtocolVersion();
    }

protected:
    virtual GetNextResult getNextAfterSetup();

    virtual std::unique_ptr<executor::TaskExecutorCursor> establishCursor();

    /**
     * Inspects the cursor to see if it set any vars, and propagates their definitions to the
     * ExpressionContext. For now, we only expect SEARCH_META to be defined.
     */
    void tryToSetSearchMetaVar();

private:
    /**
     * Does some common setup and checks, then calls 'getNextAfterSetup()' if appropriate.
     */
    GetNextResult doGetNext() final;
    boost::optional<BSONObj> _getNext();

    bool shouldReturnEOF() const;

    InternalSearchMongotRemoteSpec _spec;

    std::shared_ptr<executor::TaskExecutor> _taskExecutor;

    /**
     * Sort key generator used to populate $sortKey. Has a value iff '_sortSpec' has a value.
     */
    boost::optional<SortKeyGenerator> _sortKeyGen;
    // Store the cursorId. We need to store it on the document source because the id on the
    // TaskExecutorCursor will be set to zero after the final getMore after the cursor is
    // exhausted.
    boost::optional<CursorId> _cursorId{boost::none};

    long long _docsReturned = 0;

    /**
     * SearchIdLookupMetrics between MongotRemote & SearchIdLookup Stages.
     * The state is shared between these two document sources because SearchIdLookup
     * computes the document id lookup success rate, and MongotRemote uses it to make decisions
     * about the batch size it requests for search responses.
     * Note, this pointer could be null, and must be set before use.
     */
    std::shared_ptr<SearchIdLookupMetrics> _searchIdLookupMetrics;

    std::shared_ptr<InternalSearchMongotRemoteSharedState> _sharedState;
};

}  // namespace mongo::exec::agg
