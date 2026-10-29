// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/record_id.h"
#include "mongo/platform/int128.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstdint>

#include <boost/optional/optional.hpp>

namespace mongo {
// TODO SERVER-115201: Break up the utils not to cross modules
namespace [[MONGO_MOD_NEEDS_REPLACEMENT]] change_stream_pre_image_id_util {

/**
 * Parses the 'ts' field from the 'ChangeStreamPreImageId' associated with the 'rid'. The 'rid' MUST
 * be generated from a pre-image, otherwise the behavior of this method is undefined.
 */
Timestamp getPreImageTimestamp(const RecordId& rid);

/**
 * Parses the 'ts' and 'applyOpsIndex' fields from the 'ChangeStreamPreImageId' associated with the
 * 'rid'. The 'rid' MUST be generated from a pre-image, otherwise the behavior of this method is
 * undefined.
 */
std::pair<Timestamp, int64_t> getPreImageTimestampAndApplyOpsIndex(const RecordId& rid);

/**
 * Converts the 'ts' and 'applyOpsIndex' fields from the 'rid' into a numeric value, for easier
 * arithmetic. The 'rid' MUST be generated from a pre-image.
 * In the resulting numeric value, the 'Timestamp' part will be more significant than the
 * 'applyOpsIndex' part, i.e. the resulting numeric values sort first by their 'Timestamp' part,
 * then by their 'applyOpsIndex' part.
 */
uint128_t timestampAndApplyOpsIndexToNumber(const RecordId& rid);

/**
 * Converts the 'ts' and 'applyOpsIndex' values into a numeric value, for easier arithmetic.
 * In the resulting numeric value, the 'Timestamp' part will be more significant than the
 * 'applyOpsIndex' part, i.e. the resulting numeric values sort first by their 'Timestamp' part,
 * then by their 'applyOpsIndex' part.
 */
uint128_t timestampAndApplyOpsIndexToNumber(Timestamp ts, int64_t applyOpsIndex);

/**
 * Converts the numeric value back into its 'ts' and 'applyOpsIndex' components.
 */
std::pair<Timestamp, int64_t> timestampAndApplyOpsIndexFromNumber(uint128_t value);

/**
 * Converts the 'ChangeStreamPreImageId' to its 'RecordId' equivalent.
 */
RecordId toRecordId(const ChangeStreamPreImageId& id);

/**
 * Construct a 'RecordIdBound' for the specified combination of 'nsUUID', Timestamp 'ts' and
 * 'applyOpsIndex'.
 */
RecordIdBound getPreImageRecordIdForNsTimestampApplyOpsIndex(const UUID& nsUUID,
                                                             Timestamp ts,
                                                             int64_t applyOpsIndex);

/**
 * A given pre-images collection consists of segments of pre-images generated from different UUIDs.
 * Returns the absolute min/max RecordIdBounds for the segment of pre-images generated from
 * 'nsUUID'.
 */
RecordIdBound getAbsoluteMinPreImageRecordIdBoundForNs(const UUID& nsUUID);
RecordIdBound getAbsoluteMaxPreImageRecordIdBoundForNs(const UUID& nsUUID);

UUID getPreImageNsUUID(const BSONObj& preImageObj);

/**
 * Truncate ranges must be consistent data - no record within a truncate range should be written
 * after the truncate. Otherwise, the data viewed by an open change stream could be corrupted,
 * only seeing part of the range post truncate. The node can either be a secondary or primary at
 * this point. Restrictions must be in place to ensure consistent ranges in both scenarios.
 * - Primaries can't truncate past the 'allDurable' Timestamp. 'allDurable' guarantees out-of-order
 * writes on the primary don't leave oplog holes.
 *
 * - Secondaries can't truncate past the 'lastApplied' timestamp. Within an oplog batch, entries are
 * applied out of order, thus truncate markers may be created before the entire batch is finished.
 * The 'allDurable' Timestamp is not sufficient given it is computed from within WT, which won't
 * always know there are entries with opTime < 'allDurable' which have yet to enter the storage
 * engine during secondary oplog application.
 *
 * Returns the maximum 'ts' a pre-image is allowed to have in order to be safely truncated.
 */
Timestamp getMaxTSEligibleForTruncate(OperationContext* opCtx);
}  // namespace change_stream_pre_image_id_util
}  // namespace mongo
