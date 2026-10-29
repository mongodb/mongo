// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/sort_stage.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
class SampleStage final : public Stage {
public:
    SampleStage(std::string_view stageName,
                const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                long long size);

private:
    GetNextResult doGetNext() final;

    // Uses a $sort stage to randomly sort the documents.
    boost::intrusive_ptr<SortStage> _sortStage;

    // Then number of documents to sample.
    const long long _size;
};
}  // namespace mongo::exec::agg
