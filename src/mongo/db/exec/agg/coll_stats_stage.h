// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_coll_stats_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

/**
 * This class handles the execution part of the coll stats aggregation stage and
 * is part of the execution pipeline. Its construction is based on
 * DocumentSourceCollStats, which handles the optimization part.
 */
class CollStatsStage final : public Stage {
public:
    static BSONObj makeStatsForNs(const boost::intrusive_ptr<ExpressionContext>&,
                                  const NamespaceString&,
                                  const DocumentSourceCollStatsSpec&,
                                  const boost::optional<BSONObj>& filterObj = boost::none);

    CollStatsStage(std::string_view stageName,
                   const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                   DocumentSourceCollStatsSpec collStatsSpec);

private:
    GetNextResult doGetNext() final;

    // The raw object given to $collStats containing user specified options.
    DocumentSourceCollStatsSpec _collStatsSpec;
    bool _finished = false;
};
}  // namespace mongo::exec::agg
