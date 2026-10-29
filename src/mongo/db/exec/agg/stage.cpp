// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/stage.h"

#include <string_view>

namespace mongo {
namespace exec {
namespace agg {


Stage::Stage(std::string_view stageName, const boost::intrusive_ptr<ExpressionContext>& pCtx)
    : pSource(nullptr), pExpCtx(pCtx), _commonStats(stageName) {
    if (pExpCtx->shouldCollectDocumentSourceExecStats()) {
        if (pExpCtx->getQueryKnobConfiguration().getMeasureQueryExecutionTimeInNanoseconds()) {
            _commonStats.executionTime.precision = QueryExecTimerPrecision::kNanos;
        } else {
            _commonStats.executionTime.precision = QueryExecTimerPrecision::kMillis;
        }
    }
}

Document Stage::getExplainOutput(const query_shape::SerializationOptions&) const {
    MutableDocument doc;
    doc.addField("nReturned", Value(static_cast<long long>(_commonStats.advanced)));

    if (_commonStats.executionTime.precision == QueryExecTimerPrecision::kMillis) {
        doc.addField(
            "executionTimeMillisEstimate",
            Value(durationCount<Milliseconds>(_commonStats.executionTime.executionTimeEstimate)));
    } else if (_commonStats.executionTime.precision == QueryExecTimerPrecision::kNanos) {
        doc.addField(
            "executionTimeMillisEstimate",
            Value(durationCount<Milliseconds>(_commonStats.executionTime.executionTimeEstimate)));
        doc.addField(
            "executionTimeMicros",
            Value(durationCount<Microseconds>(_commonStats.executionTime.executionTimeEstimate)));
        doc.addField(
            "executionTimeNanos",
            Value(durationCount<Nanoseconds>(_commonStats.executionTime.executionTimeEstimate)));
    }
    return doc.freeze();
}

}  // namespace agg
}  // namespace exec
}  // namespace mongo
