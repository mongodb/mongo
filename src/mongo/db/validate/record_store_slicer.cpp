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

    SliceStrategy strategy{SliceStrategy::kOneSlice};
    if (sliceCount <= 1) {
        return {{firstRecord->id}, strategy};
    }
    std::vector<RecordId> pivots{firstRecord->id};

    if (firstRecord->id.isStr()) {
        // Attempt to open a random cursor. If the API returns a null cursor, fallthrough to return
        // only the first recordId as a pivot.
        if (const auto randomCursor = recordStore.getRandomCursor(opCtx, ru)) {
            strategy = SliceStrategy::kRandomCursorDistribution;
            // Ask for one sample per interior boundary so the cursor spreads them across the store
            // rather than drawing each independently. Samples arrive unordered and may repeat.
            pivots.reserve(sliceCount);
            for (size_t i = 1; i < sliceCount; ++i) {
                if (const auto sample = randomCursor->next()) {
                    pivots.push_back(sample->id);
                }
            }
        }
    } else if (firstRecord->id.isLong()) {
        const int64_t first = firstRecord->id.getLong();
        const auto reverseCursor = recordStore.getCursor(opCtx, ru, /*forward=*/false);
        const auto lastRecord = reverseCursor->next();
        const int64_t last = lastRecord->id.getLong();
        if (last <= first) {
            return {pivots, strategy};
        }

        const int64_t stride = (last - first) / static_cast<int64_t>(sliceCount);
        if (stride <= 0) {
            return {pivots, strategy};
        }

        strategy = SliceStrategy::kArithmeticStride;
        pivots.reserve(sliceCount);
        for (size_t i = 1; i < sliceCount; ++i) {
            pivots.emplace_back(first + stride * static_cast<int64_t>(i));
        }
    }
    std::sort(pivots.begin(), pivots.end());
    pivots.erase(std::unique(pivots.begin(), pivots.end()), pivots.end());
    return {pivots, strategy};
}

}  // namespace collection_validation
}  // namespace mongo
