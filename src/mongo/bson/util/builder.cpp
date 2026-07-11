// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/util/builder.h"

namespace mongo {

template class BasicBufBuilder<SharedBufferAllocator>;
template class BasicBufBuilder<allocator_aware::SharedBufferAllocator<tracking::Allocator<void>>>;
template class BasicBufBuilder<SharedBufferFragmentAllocator>;
template class BasicBufBuilder<UniqueBufferAllocator>;
template class StackBufBuilderBase<StackSizeDefault>;
template class StringBuilderImpl<BufBuilder>;
template class StringBuilderImpl<StackBufBuilderBase<StackSizeDefault>>;

}  // namespace mongo
