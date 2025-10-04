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
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_function_exec.h"
#include "mongo/db/query/query_shape/serialization_options.h"

namespace mongo::exec::agg {

class InternalSetWindowFieldsStage final : public Stage {
public:
    InternalSetWindowFieldsStage(
        StringData stageName,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::optional<boost::intrusive_ptr<Expression>>& partitionBy,
        const boost::optional<SortPattern>& sortBy,
        const std::vector<WindowFunctionStatement>& outputFields);

    void setSource(Stage* source) final {
        pSource = source;
        _iterator.setSource(source);
    }

    bool usedDisk() const final {
        return _iterator.usedDisk();
    };

    const SpecificStats* getSpecificStats() const final {
        return &_stats;
    }

    Document getExplainOutput(
        const SerializationOptions& opts = SerializationOptions{}) const final;

private:
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
};


}  // namespace mongo::exec::agg
