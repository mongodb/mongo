// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/s/document_source_analyze_shard_key_read_write_distribution_gen.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::exec::agg {

class AnalyzeShardKeyReadWriteDistributionStage final : public Stage {
public:
    AnalyzeShardKeyReadWriteDistributionStage(
        std::string_view stageName,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        analyze_shard_key::DocumentSourceAnalyzeShardKeyReadWriteDistributionSpec spec);

private:
    GetNextResult doGetNext() final;

    analyze_shard_key::DocumentSourceAnalyzeShardKeyReadWriteDistributionSpec _spec;
    bool _finished = false;
};
}  // namespace mongo::exec::agg
