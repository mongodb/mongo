// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace mongo {
namespace collection_validation {

enum class SliceStrategy {
    kEmptyStore,        // Record store is empty
    kOneSlice,          // Record store cannot be sliced
    kArithmeticStride,  // Integer-keyed slices are computed by using the min and max record ID and
                        // a expected stride
    kRandomCursorDistribution  // Random cursor is used to find pivot points with expected uniform
                               // distribution
};

std::string_view toString(SliceStrategy strategy);

struct SlicePivotResults {
    std::vector<RecordId> pivots;
    SliceStrategy strategy;
};

/**
 * Returns the pivots splitting 'recordStore' into at most 'sliceCount' slices, by placing evenly
 * strided boundaries across its RecordId range. The bounds of that range are read off the record
 * store itself using a forward and reverse cursor.
 *
 * Returns a single pivot - one slice covering the whole record store - when the range cannot be
 * strided: a record store containing one record, 'sliceCount' of one, or a record store
 * implementation that does not support slicing. Returns an empty vector when the record store holds
 * no records at all, since there is then nothing to traverse and no slice to describe.
 */
[[nodiscard]] SlicePivotResults computeSlicePivots(OperationContext* opCtx,
                                                   RecoveryUnit& ru,
                                                   const RecordStore& recordStore,
                                                   size_t sliceCount);

}  // namespace collection_validation
}  // namespace mongo
