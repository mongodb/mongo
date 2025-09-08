/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/record_id.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace change_stream_pre_image_util {
/**
 * If 'expireAfterSeconds' is defined for pre-images, returns its value. Otherwise, returns
 * boost::none.
 */
boost::optional<Seconds> getExpireAfterSeconds(OperationContext* opCtx);

/**
 * If 'expireAfterSeconds' is defined for pre-images, returns the 'date' at which all pre-images
 * with 'operationTime' <= 'date' are expired. Otherwise, returns boost::none.
 */
boost::optional<Date_t> getPreImageOpTimeExpirationDate(OperationContext* opCtx,
                                                        Date_t currentTime);

/**
 * Parses the 'ts' field from the 'ChangeStreamPreImageId' associated with the 'rid'. The 'rid' MUST
be
 * generated from a pre-image.
 */
Timestamp getPreImageTimestamp(const RecordId& rid);

RecordId toRecordId(ChangeStreamPreImageId id);

/**
 * A given pre-images collection consists of segments of pre-images generated from different UUIDs.
 * Returns the absolute min/max RecordIdBounds for the segment of pre-images generated from
 * 'nsUUID'.
 */
RecordIdBound getAbsoluteMinPreImageRecordIdBoundForNs(const UUID& nsUUID);
RecordIdBound getAbsoluteMaxPreImageRecordIdBoundForNs(const UUID& nsUUID);

/**
 * Truncates a pre-images collection from 'minRecordId' to 'maxRecordId' inclusive. 'bytesDeleted'
 * and 'docsDeleted' are estimates of the bytes and documents that will be truncated within the
 * provided range.
 */
void truncateRange(OperationContext* opCtx,
                   const CollectionPtr& preImagesColl,
                   const RecordId& minRecordId,
                   const RecordId& maxRecordId,
                   int64_t bytesDeleted,
                   int64_t docsDeleted);

/**
 * Truncates all pre-images with '_id.ts' <= 'expirationTimestampApproximation'.
 */
void truncatePreImagesByTimestampExpirationApproximation(
    OperationContext* opCtx,
    const CollectionAcquisition& preImagesColl,
    Timestamp expirationTimestampApproximation);

UUID getPreImageNsUUID(const BSONObj& preImageObj);

/**
 * Finds the next collection UUID in 'preImagesCollPtr' greater than 'currentNsUUID'. Returns
 * boost::none if the next collection is not found. Stores the wall time of the first record in the
 * next collection in 'firstDocWallTime'.
 */
boost::optional<UUID> findNextCollectionUUID(OperationContext* opCtx,
                                             const CollectionPtr* preImagesCollPtr,
                                             boost::optional<UUID> currentNsUUID,
                                             Date_t& firstDocWallTime);

/**
 * Returns the set of nsUUID's captured in the 'preImagesCollection'.
 */
stdx::unordered_set<UUID, UUID::Hash> getNsUUIDs(OperationContext* opCtx,
                                                 const CollectionAcquisition& preImagesCollection);

/**
 * Preferred method for getting the current time in pre-image removal code - in testing
 * environments, the 'changeStreamPreImageRemoverCurrentTime' failpoint can alter the return value.
 *
 * Returns the current time.
 */
Date_t getCurrentTimeForPreImageRemoval(OperationContext* opCtx);
}  // namespace change_stream_pre_image_util
}  // namespace mongo
