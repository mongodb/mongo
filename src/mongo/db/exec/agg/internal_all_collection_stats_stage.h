// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/document_source_internal_all_collection_stats_gen.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <deque>
#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

/**
 * This class handles the execution part of the internal all collection stats aggregation stage and
 * is part of the execution pipeline. Its construction is based on
 * DocumentSourceInternalAllCollectionStats, which handles the optimization part.
 */
class InternalAllCollectionStatsStage final : public Stage {
public:
    InternalAllCollectionStatsStage(
        std::string_view stageName,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        DocumentSourceInternalAllCollectionStatsSpec internalAllCollectionStatsSpec,
        const boost::intrusive_ptr<DocumentSourceMatch>& absorbedMatch,
        boost::optional<BSONObj> projectFilter);

private:
    GetNextResult doGetNext() final;

    // The specification object given to $_internalAllCollectionStats containing user specified
    // options.
    const DocumentSourceInternalAllCollectionStatsSpec _internalAllCollectionStatsSpec;

    boost::optional<std::deque<BSONObj>> _catalogDocs;

    // A $match stage can be absorbed in order to avoid unnecessarily computing the stats for
    // collections that do not match that predicate.
    boost::intrusive_ptr<DocumentSourceMatch> _absorbedMatch;

    // If a $project stage exists after $_internalAllCollectionStats, we will peek the BSONObj
    // associated with the $project. This BSONObj will be used to avoid calculating
    // unnecessary fields.
    boost::optional<BSONObj> _projectFilter;
};
}  // namespace mongo::exec::agg
