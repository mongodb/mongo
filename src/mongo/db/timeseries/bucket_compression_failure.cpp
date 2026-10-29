// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/bucket_compression_failure.h"

#include "mongo/base/init.h"
#include "mongo/util/uuid.h"

#include <string_view>

namespace mongo::timeseries {
namespace {
using namespace std::literals::string_view_literals;
MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(BucketCompressionFailure);

static constexpr std::string_view kUUIDFieldName = "collectionUUID"sv;
static constexpr std::string_view kBucketIdFieldName = "bucketId"sv;
static constexpr std::string_view kKeySignatureFieldName = "keySignature"sv;
}  // namespace

BucketCompressionFailure::BucketCompressionFailure(const UUID& collectionUUID,
                                                   const OID& bucketId,
                                                   std::uint32_t keySignature)
    : _collectionUUID(collectionUUID), _bucketId(bucketId), _keySignature(keySignature) {}

std::shared_ptr<const ErrorExtraInfo> BucketCompressionFailure::parse(const BSONObj& obj) {
    auto uuidSW = UUID::parse(obj[kUUIDFieldName]);
    invariant(uuidSW.getStatus());
    auto collectionUUID = uuidSW.getValue();
    return std::make_shared<BucketCompressionFailure>(
        collectionUUID,
        obj[kBucketIdFieldName].OID(),
        static_cast<std::uint32_t>(obj[kKeySignatureFieldName].safeNumberLong()));
}

void BucketCompressionFailure::serialize(BSONObjBuilder* builder) const {
    _collectionUUID.appendToBuilder(builder, kUUIDFieldName);
    builder->append(kBucketIdFieldName, _bucketId);
    builder->append(kKeySignatureFieldName, static_cast<long long>(_keySignature));
}

const UUID& BucketCompressionFailure::collectionUUID() const {
    return _collectionUUID;
}

const OID& BucketCompressionFailure::bucketId() const {
    return _bucketId;
}

std::uint32_t BucketCompressionFailure::keySignature() const {
    return _keySignature;
}

}  // namespace mongo::timeseries
