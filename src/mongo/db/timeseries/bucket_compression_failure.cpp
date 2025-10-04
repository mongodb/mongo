/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/timeseries/bucket_compression_failure.h"

#include "mongo/base/init.h"
#include "mongo/util/uuid.h"

namespace mongo::timeseries {
namespace {
MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(BucketCompressionFailure);

static constexpr StringData kUUIDFieldName = "collectionUUID"_sd;
static constexpr StringData kBucketIdFieldName = "bucketId"_sd;
static constexpr StringData kKeySignatureFieldName = "keySignature"_sd;
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
