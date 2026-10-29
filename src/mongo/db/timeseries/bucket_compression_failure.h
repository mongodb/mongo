// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <memory>

[[MONGO_MOD_PUBLIC]];

namespace mongo::timeseries {

class BucketCompressionFailure final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::TimeseriesBucketCompressionFailed;

    explicit BucketCompressionFailure(const UUID& collectionUUID,
                                      const OID& bucketId,
                                      std::uint32_t keySignature);

    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

    void serialize(BSONObjBuilder*) const override;

    const UUID& collectionUUID() const;
    const OID& bucketId() const;
    std::uint32_t keySignature() const;

private:
    UUID _collectionUUID;
    OID _bucketId;
    std::uint32_t _keySignature;
};

}  // namespace mongo::timeseries
