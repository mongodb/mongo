// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"

#include <cstddef>
#include <vector>

namespace mongo {
namespace collection_validation {


/**
 * Returns the pivots splitting 'recordStore' into at most 'sliceCount' slices, by placing evenly
 * strided boundaries across its RecordId range. The bounds of that range are read off the record
 * store itself using a forward and reverse cursor.
 *
 * Returns a single pivot - one slice covering the whole record store - when the range cannot be
 * strided: a single-record store, a 'sliceCount' of one, or string-formatted RecordIds, which have
 * no arithmetic to stride over. Returns an empty vector when the record store holds no records at
 * all, since there is then nothing to traverse and no slice to describe.
 */
std::vector<RecordId> computeSlicePivots(OperationContext* opCtx,
                                         RecoveryUnit& ru,
                                         const RecordStore& recordStore,
                                         size_t sliceCount);

}  // namespace collection_validation
}  // namespace mongo
