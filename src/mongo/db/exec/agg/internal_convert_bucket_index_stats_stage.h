// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/timeseries_index_conversion_options.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::exec::agg {

class InternalConvertBucketIndexStatsStage final : public Stage {
public:
    InternalConvertBucketIndexStatsStage(std::string_view stageName,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         const TimeseriesIndexConversionOptions& timeseriesOptions);

private:
    GetNextResult doGetNext() final;
    TimeseriesIndexConversionOptions _timeseriesOptions;
};

}  // namespace mongo::exec::agg
