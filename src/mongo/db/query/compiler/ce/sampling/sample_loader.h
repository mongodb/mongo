// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/ce/sampling/sample.h"
#include "mongo/db/query/compiler/ce/sampling/sample_spec.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/util/modules.h"

namespace mongo::ce {

/**
 * This class coordinates the loading of samples.
 */
class SampleLoader {
public:
    SampleLoader(OperationContext* opCtx,
                 const MultipleCollectionAccessor& collections,
                 PlanYieldPolicy::YieldPolicy yieldPolicy,
                 boost::intrusive_ptr<const ExpressionContext> customerQueryExpCtx);

    virtual ~SampleLoader() = default;

    SampleLoader(const SampleLoader&) = delete;
    SampleLoader& operator=(const SampleLoader&) = delete;

    /**
     * Returns the loaded sample, or a non-OK status if the sample could not be loaded.
     * NoSuchKey means no sample was found for this loader, so the caller may fall back to another
     * loader.
     */
    virtual StatusWith<Sample> tryLoad(const SampleSpec& spec) = 0;

protected:
    /**
     * The technique this loader produces samples with. Stamped onto every sample it returns, so
     * that callers never have to infer after the fact how a sample was obtained.
     */
    virtual SamplingTechniqueEnum technique(const SampleSpec& spec) const = 0;

    OperationContext* _opCtx;
    const MultipleCollectionAccessor& _collections;
    PlanYieldPolicy::YieldPolicy _yieldPolicy;
    boost::intrusive_ptr<const ExpressionContext> _customerQueryExpCtx;
};

}  // namespace mongo::ce
