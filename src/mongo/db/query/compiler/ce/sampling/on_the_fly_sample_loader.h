// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/ce/sampling/sample.h"
#include "mongo/db/query/compiler/ce/sampling/sample_loader.h"
#include "mongo/db/query/compiler/ce/sampling/sample_spec.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo::ce {

/**
 * Generates a sample on the fly using the technique it was constructed with.
 */
class OnTheFlySampleLoader final : public SampleLoader {
public:
    OnTheFlySampleLoader(OperationContext* opCtx,
                         const MultipleCollectionAccessor& collections,
                         PlanYieldPolicy::YieldPolicy yieldPolicy,
                         boost::intrusive_ptr<const ExpressionContext> customerQueryExpCtx,
                         SamplingTechniqueEnum technique);

    StatusWith<Sample> tryLoad(const SampleSpec& spec) override;

protected:
    SamplingTechniqueEnum technique(const SampleSpec&) const override {
        return _technique;
    }

private:
    const SamplingTechniqueEnum _technique;
};

std::unique_ptr<OnTheFlySampleLoader> makeOnTheFlySampleLoader(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    boost::intrusive_ptr<const ExpressionContext> customerQueryExpCtx,
    const SampleSpec& spec);

}  // namespace mongo::ce
