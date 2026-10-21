// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate/record_store_slicer.h"

#include "mongo/db/storage/record_store.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace mongo {
namespace collection_validation {
namespace {

SlicePivotResults _computeSlicePivots(const RecordId& firstRecordId,
                                      const RecordId& lastRecordId,
                                      size_t sliceCount) {
    SlicePivotResults oneSlice{{firstRecordId}, SliceStrategy::kOneSlice};
    std::vector<RecordId> pivots{firstRecordId};

    if (firstRecordId.isNull() || lastRecordId.isNull() || sliceCount <= 1) {
        return oneSlice;
    }

    // TODO(SERVER-134015): String-formatted RecordIds, used by clustered collections, have no
    // arithmetic to stride over, so they traverse as a single slice for now. Interpolating over the
    // key bytes would restore parallelism for clustered and timeseries collections.
    if (!firstRecordId.isLong() || !lastRecordId.isLong()) {
        return oneSlice;
    }

    const int64_t first = firstRecordId.getLong();
    const int64_t last = lastRecordId.getLong();
    if (last <= first) {
        return oneSlice;
    }

    const int64_t stride = (last - first) / static_cast<int64_t>(sliceCount);
    if (stride <= 0) {
        return oneSlice;
    }

    pivots.reserve(sliceCount);
    for (size_t i = 1; i < sliceCount; ++i) {
        pivots.emplace_back(first + stride * static_cast<int64_t>(i));
    }
    std::sort(pivots.begin(), pivots.end());
    pivots.erase(std::unique(pivots.begin(), pivots.end()), pivots.end());
    return {std::move(pivots), SliceStrategy::kArithmeticStride};
}

}  // namespace

std::string_view toString(SliceStrategy strategy) {
    switch (strategy) {
        case SliceStrategy::kArithmeticStride:
            return "arithmeticStride";
        case SliceStrategy::kRandomCursorDistribution:
            return "randomCursorDistribution";
        case SliceStrategy::kOneSlice:
            return "oneSlice";
        case SliceStrategy::kEmptyStore:
            return "emptyStore";
    }
    MONGO_UNREACHABLE;
}

SlicePivotResults computeSlicePivots(OperationContext* opCtx,
                                     RecoveryUnit& ru,
                                     const RecordStore& recordStore,
                                     size_t sliceCount) {
    // An empty record store has no first record, so a forward cursor that comes back empty is how
    // emptiness is detected. There is nothing to traverse, so describe no slices at all.
    const auto forwardCursor = recordStore.getCursor(opCtx, ru, /*forward=*/true);
    const auto firstRecord = forwardCursor->next();
    if (!firstRecord) {
        return {{}, SliceStrategy::kEmptyStore};
    }

    if (sliceCount <= 1) {
        return {{firstRecord->id}, SliceStrategy::kOneSlice};
    }

    const auto reverseCursor = recordStore.getCursor(opCtx, ru, /*forward=*/false);
    const auto lastRecord = reverseCursor->next();
    return _computeSlicePivots(
        firstRecord->id, lastRecord ? lastRecord->id : RecordId(), sliceCount);
}

}  // namespace collection_validation
}  // namespace mongo
