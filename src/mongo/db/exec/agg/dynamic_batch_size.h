// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include <cstddef>

namespace mongo {
namespace exec {
namespace agg {

struct DynamicBatchSize {
    // 0: No doc-count limit (bufferSize only batching). Value is mutable and can be updated
    // per-batch.
    size_t docLimit{0};
};

}  // namespace agg
}  // namespace exec
}  // namespace mongo
