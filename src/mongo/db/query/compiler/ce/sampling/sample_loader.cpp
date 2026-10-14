// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/sampling/sample_loader.h"

#include <utility>

namespace mongo::ce {

SampleLoader::SampleLoader(OperationContext* opCtx,
                           const MultipleCollectionAccessor& collections,
                           PlanYieldPolicy::YieldPolicy yieldPolicy,
                           boost::intrusive_ptr<const ExpressionContext> customerQueryExpCtx)
    : _opCtx(opCtx),
      _collections(collections),
      _yieldPolicy(yieldPolicy),
      _customerQueryExpCtx(std::move(customerQueryExpCtx)) {}

}  // namespace mongo::ce
