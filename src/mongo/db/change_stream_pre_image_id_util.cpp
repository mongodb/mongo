/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/db/change_stream_pre_image_id_util.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/version_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace change_stream_pre_image_id_util {
namespace {
using namespace std::literals::string_view_literals;
static constexpr auto kTopLevelFieldName = "ridAsBSON"sv;

// Number of valid bits for 'applyOpsIndex' value. The highest (64th) bit would be the sign bit, but
// 'applyOpsIndex' values must always be >= 0.
static constexpr uint64_t kApplyOpsIndexBits = 63;

// Bit mask for masking out the highest bit of an 'applyOpsIndex' value.
static constexpr uint128_t kBitMaskForApplyOpsIndex = ~(uint64_t(1) << kApplyOpsIndexBits);
}  // namespace

Timestamp getPreImageTimestamp(const RecordId& rid) {
    auto ridAsNestedBSON = record_id_helpers::toBSONAs(rid, kTopLevelFieldName);

    // 'toBSONAs()' discards type bits of the underlying KeyString of the RecordId. However, since
    // the 'ts' field of 'ChangeStreamPreImageId' is distinct CType::kTimestamp, type bits aren't
    // necessary to obtain the original value.
    auto ridBSON = ridAsNestedBSON.getObjectField(kTopLevelFieldName);

    auto tsElem = ridBSON.getField(ChangeStreamPreImageId::kTsFieldName);
    invariant(!tsElem.eoo());
    return tsElem.timestamp();
}

std::pair<Timestamp, int64_t> getPreImageTimestampAndApplyOpsIndex(const RecordId& rid) {
    auto ridAsNestedBSON = record_id_helpers::toBSONAs(rid, kTopLevelFieldName);

    // 'toBSONAs()' discards type bits of the underlying KeyString of the RecordId. However, since
    // the 'ts' field of 'ChangeStreamPreImageId' is distinct CType::kTimestamp, type bits aren't
    // necessary to obtain the original value.
    auto ridBSON = ridAsNestedBSON.getObjectField(kTopLevelFieldName);

    auto tsElem = ridBSON.getField(ChangeStreamPreImageId::kTsFieldName);
    invariant(!tsElem.eoo());

    auto applyOpsElem = ridBSON.getField(ChangeStreamPreImageId::kApplyOpsIndexFieldName);
    invariant(!applyOpsElem.eoo());

    return {tsElem.timestamp(), applyOpsElem.numberLong()};
}

uint128_t timestampAndApplyOpsIndexToNumber(const RecordId& rid) {
    auto [ts, applyOpsIndex] = getPreImageTimestampAndApplyOpsIndex(rid);
    return timestampAndApplyOpsIndexToNumber(ts, applyOpsIndex);
}

uint128_t timestampAndApplyOpsIndexToNumber(Timestamp ts, int64_t applyOpsIndex) {
    // 'applyOpsIndex' cannot be negative. It is stored as an int64_t inside the records,
    // but must have a value >= 0.
    invariant(applyOpsIndex >= 0);

    // The higher 65 bits of the resulting uint128_t value are the timestamp bits. The highest bit
    // is always cleared.
    uint128_t value = static_cast<uint128_t>(ts.asULL()) << kApplyOpsIndexBits;

    // The lower 63 bits of the resulting uint128_t value are the applyOpsIndex bits. The maximum
    // possible value of the applyOpsIndex has only 63 bits set. If the 'applyOpsIndex' part in the
    // lower half of the uint128_t value overflows, we want it to leak into the lower bits of the
    // upper half.
    value |= uint128_t(applyOpsIndex) & kBitMaskForApplyOpsIndex;

    return value;
}

std::pair<Timestamp, int64_t> timestampAndApplyOpsIndexFromNumber(uint128_t value) {
    // The 'applyOpsIndex' part are the lower 63 bits of the value.
    int64_t applyOpsIndex = static_cast<int64_t>(value & kBitMaskForApplyOpsIndex);

    // The timestamp part is contained in the upper 64 bits of the value.
    return {Timestamp(static_cast<unsigned long long>(value >> kApplyOpsIndexBits)), applyOpsIndex};
}

RecordId toRecordId(const ChangeStreamPreImageId& id) {
    return record_id_helpers::keyForElem(
        BSON(ChangeStreamPreImage::kIdFieldName << id.toBSON()).firstElement());
}

RecordIdBound getPreImageRecordIdForNsTimestampApplyOpsIndex(const UUID& nsUUID,
                                                             Timestamp ts,
                                                             int64_t applyOpsIndex) {
    return RecordIdBound(toRecordId(ChangeStreamPreImageId(nsUUID, ts, applyOpsIndex)));
}

RecordIdBound getAbsoluteMinPreImageRecordIdBoundForNs(const UUID& nsUUID) {
    return getPreImageRecordIdForNsTimestampApplyOpsIndex(nsUUID, Timestamp(), 0);
}

RecordIdBound getAbsoluteMaxPreImageRecordIdBoundForNs(const UUID& nsUUID) {
    return getPreImageRecordIdForNsTimestampApplyOpsIndex(
        nsUUID, Timestamp::max(), std::numeric_limits<int64_t>::max());
}

UUID getPreImageNsUUID(const BSONObj& preImageObj) {
    auto parsedUUID = UUID::parse(preImageObj[ChangeStreamPreImage::kIdFieldName]
                                      .Obj()[ChangeStreamPreImageId::kNsUUIDFieldName]);
    tassert(7027400, "Pre-image collection UUID must be of UUID type", parsedUUID.isOK());
    return std::move(parsedUUID.getValue());
}

Timestamp getMaxTSEligibleForTruncate(OperationContext* opCtx) {
    Timestamp allDurable = opCtx->getServiceContext()->getStorageEngine()->getAllDurableTimestamp();
    auto lastAppliedOpTime = repl::ReplicationCoordinator::get(opCtx)->getMyLastAppliedOpTime();
    return std::min(lastAppliedOpTime.getTimestamp(), allDurable);
}
}  // namespace change_stream_pre_image_id_util
}  // namespace mongo
