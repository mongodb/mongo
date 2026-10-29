// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
class PlanCacheStatsStage final : public Stage {
public:
    PlanCacheStatsStage(std::string_view stageName,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        const boost::intrusive_ptr<DocumentSourceMatch>& absorbedMatch);

private:
    GetNextResult doGetNext() final;

    // If running through mongos in a sharded cluster, stores the "host:port" string so that it can
    // be appended to each plan cache entry document.
    std::string _hostAndPort;

    // If running through mongos in a sharded cluster, stores the shard name so that it can be
    // appended to each plan cache entry document.
    std::string _shardName;

    // The result set for this change is produced through the mongo process interface on the first
    // call to getNext(), and then held by this data member.
    std::vector<BSONObj> _results;

    // Whether '_results' has been populated yet.
    bool _haveRetrievedStats = false;

    // Used to spool out '_results' as calls to getNext() are made.
    std::vector<BSONObj>::iterator _resultsIter;

    // $planCacheStats can push a match down into the plan cache layer, in order to avoid copying
    // the entire contents of the cache.
    boost::intrusive_ptr<DocumentSourceMatch> _absorbedMatch;
};
}  // namespace mongo::exec::agg
