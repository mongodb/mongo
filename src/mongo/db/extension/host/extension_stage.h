// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/extension/shared/get_next_result.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/executable_agg_stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
namespace exec {
namespace agg {
/**
 * A Stage implementation for an extension aggregation stage. ExtensionStage is a facade around
 * handles to extension API objects.
 */
class ExtensionStage final : public Stage {
public:
    ExtensionStage(std::string_view name,
                   const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                   extension::ExecAggStageHandle execAggStageHandle);
    Document getExplainOutput(const query_shape::SerializationOptions& opts =
                                  query_shape::SerializationOptions{}) const override;

    bool supportsDynamicBatchSize() const override {
        return true;
    }

    void setDynamicBatchSize(DynamicBatchSize* dbs) override {
        _dynamicBatchSize = dbs;
    }

private:
    void setSource(Stage* source) override;
    GetNextResult doGetNext() final;

    extension::ExecAggStageHandle _execAggStageHandle{nullptr};
    extension::ExecAggStageHandle _sourceAggStageHandle{nullptr};
    extension::ExtensionGetNextResult _lastGetNextResult;
    DynamicBatchSize* _dynamicBatchSize{nullptr};
};
}  // namespace agg
}  // namespace exec
}  // namespace mongo
