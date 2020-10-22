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
namespace {

const BSONField<bool> kNoBalance("noBalance");

}  // namespace

const NamespaceString CollectionType::ConfigNS("config.collections");

const BSONField<BSONObj> CollectionType::defaultCollation("defaultCollation");
const BSONField<bool> CollectionType::unique("unique");
const BSONField<UUID> CollectionType::uuid("uuid");
const BSONField<std::string> CollectionType::distributionMode("distributionMode");
const BSONField<ReshardingFields> CollectionType::reshardingFields("reshardingFields");

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

    {
        BSONElement collDefaultCollation;
        Status status =
            bsonExtractTypedField(source, defaultCollation.name(), Object, &collDefaultCollation);
        if (status.isOK()) {
            BSONObj obj = collDefaultCollation.Obj();
            if (obj.isEmpty()) {
                return Status(ErrorCodes::BadValue, "empty defaultCollation");
            }

            coll._defaultCollation = obj.getOwned();
        } else if (status != ErrorCodes::NoSuchKey) {
            return status;
        }
    }

    {
        bool collUnique;
        Status status = bsonExtractBooleanField(source, unique.name(), &collUnique);
        if (status.isOK()) {
            coll._unique = collUnique;
        } else if (status == ErrorCodes::NoSuchKey) {
            // Key uniqueness can be missing in which case it is presumed false
        } else {
            return status;
        }
    }

    {
        BSONElement uuidElem;
        Status status = bsonExtractField(source, uuid.name(), &uuidElem);
        if (status.isOK()) {
            auto swUUID = UUID::parse(uuidElem);
            if (!swUUID.isOK()) {
                return swUUID.getStatus();
            }
            coll._uuid = swUUID.getValue();
        } else if (status == ErrorCodes::NoSuchKey) {
            // UUID can be missing in 3.6, because featureCompatibilityVersion can be 3.4, in which
            // case it remains boost::none.
        } else {
            return status;
        }
    }

    {
        bool collNoBalance;
        Status status = bsonExtractBooleanField(source, kNoBalance.name(), &collNoBalance);
        if (status.isOK()) {
            coll._allowBalance = !collNoBalance;
        } else if (status == ErrorCodes::NoSuchKey) {
            // No balance can be missing in which case it is presumed as false
        } else {
            return status;
        }
    }

    {
        const auto reshardingFieldsElem = source.getField(reshardingFields.name());
        if (reshardingFieldsElem) {
            coll._reshardingFields =
                ReshardingFields::parse(IDLParserErrorContext("TypeCollectionReshardingFields"),
                                        reshardingFieldsElem.Obj());
        }
    }

    return StatusWith<CollectionType>(coll);
}

Status CollectionType::validate() const {
    return Status::OK();
}

BSONObj CollectionType::toBSON() const {
    BSONObjBuilder builder;
    serialize(&builder);

    if (!_defaultCollation.isEmpty()) {
        builder.append(defaultCollation.name(), _defaultCollation);
    }

    if (_unique.is_initialized()) {
        builder.append(unique.name(), _unique.get());
    }

    if (_uuid.is_initialized()) {
        _uuid->appendToBuilder(&builder, uuid.name());
    }

    if (_allowBalance.is_initialized()) {
        builder.append(kNoBalance.name(), !_allowBalance.get());
    }

    if (_distributionMode) {
        if (*_distributionMode == DistributionMode::kUnsharded) {
            builder.append(distributionMode.name(), "unsharded");
        } else if (*_distributionMode == DistributionMode::kSharded) {
            builder.append(distributionMode.name(), "sharded");
        } else {
            MONGO_UNREACHABLE;
        }
    }

    if (_reshardingFields) {
        builder.append(reshardingFields.name(), _reshardingFields->toBSON());
    }

    return builder.obj();
}

std::string CollectionType::toString() const {
    return toBSON().toString();
}

void CollectionType::setEpoch(OID epoch) {
    setPre22CompatibleEpoch(std::move(epoch));
}

void CollectionType::setKeyPattern(KeyPattern keyPattern) {
    setPre50CompatibleKeyPattern(std::move(keyPattern));
}

void CollectionType::setReshardingFields(boost::optional<ReshardingFields> reshardingFields) {
    _reshardingFields = std::move(reshardingFields);
}

bool CollectionType::hasSameOptions(const CollectionType& other) const {
    return getNss() == other.getNss() &&
        SimpleBSONObjComparator::kInstance.evaluate(getKeyPattern().toBSON() ==
                                                    other.getKeyPattern().toBSON()) &&
        SimpleBSONObjComparator::kInstance.evaluate(_defaultCollation ==
                                                    other.getDefaultCollation()) &&
        *_unique == other.getUnique() && getDistributionMode() == other.getDistributionMode();
}

}  // namespace mongo
