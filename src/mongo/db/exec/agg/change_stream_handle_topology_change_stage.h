// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/s/query/exec/async_results_merger_params_gen.h"
#include "mongo/s/query/exec/merge_cursors_stage.h"
#include "mongo/util/modules.h"

#include <string_view>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
/**
 * This class handles the execution part of the change stream handle topology change aggregation
 * stage and is part of the execution pipeline. Its construction is based on
 * DocumentSourceChangeStreamHandleTopologyChange, which handles the optimization part.
 */
class ChangeStreamHandleTopologyChangeStage final : public Stage {
public:
    ChangeStreamHandleTopologyChangeStage(std::string_view stageName,
                                          const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    GetNextResult doGetNext() final;

    /**
     * Establish the new cursors and tell the RouterStageMerge about them.
     */
    void addNewShardCursors(const Document& newShardDetectedObj);

    /**
     * Open the cursors on the new shards.
     */
    std::vector<RemoteCursor> establishShardCursorsOnNewShards(const Document& newShardDetectedObj);

    boost::intrusive_ptr<MergeCursorsStage> _mergeCursors;
    BSONObj _originalAggregateCommand;

    /**
     * The cluster time at which the change stream has been opened.
     */
    Timestamp _originalResumeTokenClusterTime;
};
}  // namespace mongo::exec::agg
