// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/add_fields_projection_executor.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_function_exec.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::exec::agg {

class InternalSetWindowFieldsStage final : public Stage {
public:
    InternalSetWindowFieldsStage(
        std::string_view stageName,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::optional<boost::intrusive_ptr<Expression>>& partitionBy,
        const boost::optional<SortPattern>& sortBy,
        const std::vector<WindowFunctionStatement>& outputFields);

    bool usedDisk() const final {
        return _iterator.usedDisk();
    };

    const SpecificStats* getSpecificStats() const final {
        return &_stats;
    }

    Document getExplainOutput(const query_shape::SerializationOptions& opts =
                                  query_shape::SerializationOptions{}) const final;

private:
    void setSource(Stage* source) final {
        pSource = source;
        _iterator.setSource(source);
    }

    void initialize();

    GetNextResult doGetNext() final;

    void doDispose() final;

    void doForceSpill() final {
        _iterator.spillToDisk();
    }

    // Memory tracker is not updated directly by this class, but it is passed down to
    // PartitionIterator and WindowFunctionExec's that update their memory consumption.
    MemoryUsageTracker _memoryTracker;

    PartitionIterator _iterator;

    DocumentSourceSetWindowFieldsStats _stats;

    // std::map is necessary to guarantee iteration order - see SERVER-88080 for details.
    std::map<std::string, std::unique_ptr<WindowFunctionExec>> _executableOutputs;

    bool _eof{false};

    // Used by the failpoint to determine when to spill to disk.
    int32_t _numDocsProcessed{0};

    boost::optional<SortPattern> _sortBy;
    std::vector<WindowFunctionStatement> _outputFields;

    // _projExec and _constExprs hold references to shared ExpressionConstants.
    // doGetNext modifies the underlying Values in the ExpressionConstants.
    std::unique_ptr<projection_executor::AddFieldsProjectionExecutor> _projExec;
    std::vector<boost::intrusive_ptr<ExpressionConstant>> _constExprs;
    std::vector<WindowFunctionExec*> _orderedExecs;
};


}  // namespace mongo::exec::agg
