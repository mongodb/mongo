/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/type_collection.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

const BSONField<bool> kNoBalance("noBalance");
const BSONField<bool> kDropped("dropped");

}  // namespace

const std::string CollectionType::ConfigNS = "config.collections";

const BSONField<std::string> CollectionType::fullNs("_id");
const BSONField<OID> CollectionType::epoch("lastmodEpoch");
const BSONField<Date_t> CollectionType::updatedAt("lastmod");
const BSONField<BSONObj> CollectionType::keyPattern("key");
const BSONField<bool> CollectionType::unique("unique");

StatusWith<CollectionType> CollectionType::fromBSON(const BSONObj& source) {
    CollectionType coll;

    {
        std::string collFullNs;
        Status status = bsonExtractStringField(source, fullNs.name(), &collFullNs);
        if (!status.isOK())
            return status;

        coll._fullNs = NamespaceString{collFullNs};
    }

    {
        OID collEpoch;
        Status status = bsonExtractOIDFieldWithDefault(source, epoch.name(), OID(), &collEpoch);
        if (!status.isOK())
            return status;

        coll._epoch = collEpoch;
    }

    {
        BSONElement collUpdatedAt;
        Status status = bsonExtractTypedField(source, updatedAt.name(), Date, &collUpdatedAt);
        if (!status.isOK())
            return status;

        coll._updatedAt = collUpdatedAt.Date();
    }

    {
        bool collDropped;
        Status status = bsonExtractBooleanField(source, kDropped.name(), &collDropped);
        if (status.isOK()) {
            coll._dropped = collDropped;
        } else if (status == ErrorCodes::NoSuchKey) {
            // Dropped can be missing in which case it is presumed false
        } else {
            return status;
        }
    }

    {
        BSONElement collKeyPattern;
        Status status = bsonExtractTypedField(source, keyPattern.name(), Object, &collKeyPattern);
        if (status.isOK()) {
            BSONObj obj = collKeyPattern.Obj();
            if (obj.isEmpty()) {
                return Status(ErrorCodes::ShardKeyNotFound, "empty shard key");
            }

            coll._keyPattern = KeyPattern(obj.getOwned());
        } else if ((status == ErrorCodes::NoSuchKey) && coll.getDropped()) {
            // Sharding key can be missing if the collection is dropped
        } else {
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

    return StatusWith<CollectionType>(coll);
}

Status CollectionType::validate() const {
    // These fields must always be set
    if (!_fullNs.is_initialized()) {
        return Status(ErrorCodes::NoSuchKey, "missing ns");
    }

    if (!_fullNs->isValid()) {
        return Status(ErrorCodes::BadValue, "invalid namespace " + _fullNs->toString());
    }

    if (!_epoch.is_initialized()) {
        return Status(ErrorCodes::NoSuchKey, "missing epoch");
    }

    if (!_updatedAt.is_initialized()) {
        return Status(ErrorCodes::NoSuchKey, "missing updated at timestamp");
    }

    if (!_dropped.get_value_or(false)) {
        if (!_epoch->isSet()) {
            return Status(ErrorCodes::BadValue, "invalid epoch");
        }

        if (Date_t() == _updatedAt.get()) {
            return Status(ErrorCodes::BadValue, "invalid updated at timestamp");
        }

        if (!_keyPattern.is_initialized()) {
            return Status(ErrorCodes::NoSuchKey, "missing key pattern");
        } else {
            invariant(!_keyPattern->toBSON().isEmpty());
        }
    }

    return Status::OK();
}

BSONObj CollectionType::toBSON() const {
    BSONObjBuilder builder;

    if (_fullNs) {
        builder.append(fullNs.name(), _fullNs->toString());
    }
    builder.append(epoch.name(), _epoch.get_value_or(OID()));
    builder.append(updatedAt.name(), _updatedAt.get_value_or(Date_t()));
    builder.append(kDropped.name(), _dropped.get_value_or(false));

    // These fields are optional, so do not include them in the metadata for the purposes of
    // consuming less space on the config servers.

    if (_keyPattern.is_initialized()) {
        builder.append(keyPattern.name(), _keyPattern->toBSON());
    }

    if (_unique.is_initialized()) {
        builder.append(unique.name(), _unique.get());
    }

    if (_allowBalance.is_initialized()) {
        builder.append(kNoBalance.name(), !_allowBalance.get());
    }

    return builder.obj();
}

std::string CollectionType::toString() const {
    return toBSON().toString();
}

void CollectionType::setNs(const NamespaceString& fullNs) {
    invariant(fullNs.isValid());
    _fullNs = fullNs;
}

void CollectionType::setEpoch(OID epoch) {
    _epoch = epoch;
}

void CollectionType::setUpdatedAt(Date_t updatedAt) {
    _updatedAt = updatedAt;
}

void CollectionType::setKeyPattern(const KeyPattern& keyPattern) {
    invariant(!keyPattern.toBSON().isEmpty());
    _keyPattern = keyPattern;
}

}  // namespace mongo
