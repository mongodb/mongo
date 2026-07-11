// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index/preallocated_container_pool.h"

#include "mongo/util/decorable.h"

namespace mongo {

const OperationContext::Decoration<PreallocatedContainerPool> PreallocatedContainerPool::get =
    OperationContext::declareDecoration<PreallocatedContainerPool>();

}  // namespace mongo
