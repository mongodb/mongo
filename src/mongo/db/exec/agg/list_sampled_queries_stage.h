// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_list_sampled_queries.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
/**
 * This class handles the execution part of the list sampled queries aggregation stage and
 * is part of the execution pipeline. Its construction is based on
 * DocumentSourceListSampledQueries, which handles the optimization part.
 */
class ListSampledQueriesStage final : public Stage {
public:
    void detachFromOperationContext() final;

    void reattachToOperationContext(OperationContext* opCtx) final;

    ListSampledQueriesStage(
        std::string_view stageName,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        boost::optional<mongo::NamespaceString> nss,
        std::shared_ptr<analyze_shard_key::ListSampledQueriesSharedState> sharedState);

private:
    GetNextResult doGetNext() final;

    boost::optional<mongo::NamespaceString> _nss;
    std::shared_ptr<analyze_shard_key::ListSampledQueriesSharedState> _sharedState;
};
}  // namespace mongo::exec::agg
