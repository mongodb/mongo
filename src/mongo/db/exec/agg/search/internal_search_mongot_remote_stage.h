/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/index/sort_key_generator.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/search/internal_search_mongot_remote_spec_gen.h"
#include "mongo/db/query/search/search_query_view_spec_gen.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_cursor.h"

#include <memory>

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

class InternalSearchMongotRemoteStage : public Stage {
public:
    using SearchIdLookupMetrics = DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics;

    InternalSearchMongotRemoteStage(
        StringData stageName,
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
