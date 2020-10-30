/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/type_collection.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/assert_util.h"

namespace mongo {

const NamespaceString CollectionType::ConfigNS("config.collections");

const BSONField<std::string> CollectionType::distributionMode("distributionMode");

CollectionType::CollectionType(NamespaceString nss, OID epoch, Date_t updatedAt, UUID uuid)
    : CollectionTypeBase(std::move(nss), std::move(updatedAt)) {
    setEpoch(std::move(epoch));
    setUuid(std::move(uuid));
}

CollectionType::CollectionType(const BSONObj& obj) {
    CollectionType::parseProtected(IDLParserErrorContext("CollectionType"), obj);
    uassert(ErrorCodes::BadValue,
            str::stream() << "Invalid namespace " << getNss(),
            getNss().isValid());
    if (!getPre22CompatibleEpoch()) {
        setPre22CompatibleEpoch(OID());
    }
    uassert(ErrorCodes::NoSuchKey,
            "Shard key is missing",
            getPre50CompatibleKeyPattern() || getDropped());
}

StatusWith<CollectionType> CollectionType::fromBSON(const BSONObj& source) {
    auto swColl = [&] {
        try {
            return StatusWith<CollectionType>(CollectionType(source));
        } catch (const DBException& ex) {
            return StatusWith<CollectionType>(ex.toStatus());
        }
    }();

    if (!swColl.isOK())
        return swColl;

    CollectionType coll = std::move(swColl.getValue());

    {
        std::string collDistributionMode;
        Status status =
            bsonExtractStringField(source, distributionMode.name(), &collDistributionMode);
        if (status.isOK()) {
            if (collDistributionMode == "unsharded") {
                coll._distributionMode = DistributionMode::kUnsharded;
            } else if (collDistributionMode == "sharded") {
                coll._distributionMode = DistributionMode::kSharded;
            } else {
                return {ErrorCodes::FailedToParse,
                        str::stream() << "Unknown distribution mode " << collDistributionMode};
            }
        } else if (status == ErrorCodes::NoSuchKey) {
            // In v4.4, distributionMode can be missing in which case it is presumed "sharded"
        } else {
            return status;
        }
    }

    return StatusWith<CollectionType>(coll);
}

BSONObj CollectionType::toBSON() const {
    BSONObjBuilder builder;
    serialize(&builder);

    if (_distributionMode) {
        if (*_distributionMode == DistributionMode::kUnsharded) {
            builder.append(distributionMode.name(), "unsharded");
        } else if (*_distributionMode == DistributionMode::kSharded) {
            builder.append(distributionMode.name(), "sharded");
        } else {
            MONGO_UNREACHABLE;
        }
    }

    return builder.obj();
}

std::string CollectionType::toString() const {
    return toBSON().toString();
}

void CollectionType::setEpoch(OID epoch) {
    setPre22CompatibleEpoch(std::move(epoch));
}

void CollectionType::setUuid(UUID uuid) {
    setPre50CompatibleUuid(std::move(uuid));
}

void CollectionType::setKeyPattern(KeyPattern keyPattern) {
    setPre50CompatibleKeyPattern(std::move(keyPattern));
}

void CollectionType::setDefaultCollation(const BSONObj& defaultCollation) {
    if (!defaultCollation.isEmpty())
        setPre50CompatibleDefaultCollation(defaultCollation);
}

bool CollectionType::hasSameOptions(const CollectionType& other) const {
    return getNss() == other.getNss() &&
        SimpleBSONObjComparator::kInstance.evaluate(getKeyPattern().toBSON() ==
                                                    other.getKeyPattern().toBSON()) &&
        SimpleBSONObjComparator::kInstance.evaluate(getDefaultCollation() ==
                                                    other.getDefaultCollation()) &&
        getUnique() == other.getUnique() && getDistributionMode() == other.getDistributionMode();
}

}  // namespace mongo
