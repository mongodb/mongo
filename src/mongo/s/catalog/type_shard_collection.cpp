/**
 *    Copyright (C) 2017 10gen Inc.
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

#include "mongo/s/catalog/type_shard_collection.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/util/assert_util.h"

namespace mongo {

const std::string ShardCollectionType::ConfigNS =
    NamespaceString::kShardConfigCollectionsCollectionName.toString();

const BSONField<std::string> ShardCollectionType::ns("_id");
const BSONField<UUID> ShardCollectionType::uuid("uuid");
const BSONField<OID> ShardCollectionType::epoch("epoch");
const BSONField<BSONObj> ShardCollectionType::keyPattern("key");
const BSONField<BSONObj> ShardCollectionType::defaultCollation("defaultCollation");
const BSONField<bool> ShardCollectionType::unique("unique");
const BSONField<bool> ShardCollectionType::refreshing("refreshing");
const BSONField<Date_t> ShardCollectionType::lastRefreshedCollectionVersion(
    "lastRefreshedCollectionVersion");
const BSONField<int> ShardCollectionType::enterCriticalSectionCounter(
    "enterCriticalSectionCounter");

ShardCollectionType::ShardCollectionType(NamespaceString nss,
                                         boost::optional<UUID> uuid,
                                         OID epoch,
                                         const KeyPattern& keyPattern,
                                         const BSONObj& defaultCollation,
                                         bool unique)
    : _nss(std::move(nss)),
      _uuid(uuid),
      _epoch(std::move(epoch)),
      _keyPattern(keyPattern.toBSON()),
      _defaultCollation(defaultCollation.getOwned()),
      _unique(unique) {}

StatusWith<ShardCollectionType> ShardCollectionType::fromBSON(const BSONObj& source) {

    NamespaceString nss;
    {
        std::string ns;
        Status status = bsonExtractStringField(source, ShardCollectionType::ns.name(), &ns);
        if (!status.isOK()) {
            return status;
        }
        nss = NamespaceString{ns};
    }

    boost::optional<UUID> uuid;
    {
        BSONElement uuidElem;
        Status status = bsonExtractTypedField(
            source, ShardCollectionType::uuid.name(), BSONType::BinData, &uuidElem);
        if (status.isOK()) {
            auto uuidWith = UUID::parse(uuidElem);
            if (!uuidWith.isOK())
                return uuidWith.getStatus();
            uuid = uuidWith.getValue();
        } else if (status == ErrorCodes::NoSuchKey) {
            // The field is not set, which is okay.
        } else {
            return status;
        }
    }

    OID epoch;
    {
        BSONElement oidElem;
        Status status = bsonExtractTypedField(
            source, ShardCollectionType::epoch.name(), BSONType::jstOID, &oidElem);
        if (!status.isOK())
            return status;
        epoch = oidElem.OID();
    }

    BSONElement collKeyPattern;
    Status status = bsonExtractTypedField(
        source, ShardCollectionType::keyPattern.name(), Object, &collKeyPattern);
    if (!status.isOK()) {
        return status;
    }
    BSONObj obj = collKeyPattern.Obj();
    if (obj.isEmpty()) {
        return Status(ErrorCodes::ShardKeyNotFound,
                      str::stream() << "Empty shard key. Failed to parse: " << source.toString());
    }
    KeyPattern pattern(obj.getOwned());

    BSONObj collation;
    {
        BSONElement defaultCollation;
        Status status = bsonExtractTypedField(
            source, ShardCollectionType::defaultCollation.name(), Object, &defaultCollation);
        if (status.isOK()) {
            BSONObj obj = defaultCollation.Obj();
            if (obj.isEmpty()) {
                return Status(ErrorCodes::BadValue, "empty defaultCollation");
            }

            collation = obj.getOwned();
        } else if (status != ErrorCodes::NoSuchKey) {
            return status;
        }
    }

    bool unique;
    {
        Status status =
            bsonExtractBooleanField(source, ShardCollectionType::unique.name(), &unique);
        if (!status.isOK()) {
            return status;
        }
    }

    ShardCollectionType shardCollectionType(
        std::move(nss), uuid, std::move(epoch), pattern, collation, unique);

    // Below are optional fields.

    {
        bool refreshing;
        Status status =
            bsonExtractBooleanField(source, ShardCollectionType::refreshing.name(), &refreshing);
        if (status.isOK()) {
            shardCollectionType.setRefreshing(refreshing);
        } else if (status == ErrorCodes::NoSuchKey) {
            // The field is not set yet, which is okay.
        } else {
            return status;
        }
    }

    {
        if (!source[lastRefreshedCollectionVersion.name()].eoo()) {
            auto statusWithLastRefreshedCollectionVersion =
                ChunkVersion::parseFromBSONWithFieldAndSetEpoch(
                    source, lastRefreshedCollectionVersion.name(), epoch);
            if (!statusWithLastRefreshedCollectionVersion.isOK()) {
                return statusWithLastRefreshedCollectionVersion.getStatus();
            }
            shardCollectionType.setLastRefreshedCollectionVersion(
                std::move(statusWithLastRefreshedCollectionVersion.getValue()));
        }
    }

    return shardCollectionType;
}

BSONObj ShardCollectionType::toBSON() const {
    BSONObjBuilder builder;

    builder.append(ns.name(), _nss.ns());
    if (_uuid) {
        _uuid->appendToBuilder(&builder, uuid.name());
    }
    builder.append(epoch.name(), _epoch);
    builder.append(keyPattern.name(), _keyPattern.toBSON());

    if (!_defaultCollation.isEmpty()) {
        builder.append(defaultCollation.name(), _defaultCollation);
    }

    builder.append(unique.name(), _unique);

    if (_refreshing) {
        builder.append(refreshing.name(), _refreshing.get());
    }
    if (_lastRefreshedCollectionVersion) {
        builder.appendTimestamp(lastRefreshedCollectionVersion.name(),
                                _lastRefreshedCollectionVersion->toLong());
    }

    return builder.obj();
}

std::string ShardCollectionType::toString() const {
    return toBSON().toString();
}

void ShardCollectionType::setUUID(UUID uuid) {
    _uuid = uuid;
}

void ShardCollectionType::setNss(NamespaceString nss) {
    invariant(nss.isValid());
    _nss = std::move(nss);
}

void ShardCollectionType::setEpoch(OID epoch) {
    invariant(epoch.isSet());
    _epoch = std::move(epoch);
}

void ShardCollectionType::setKeyPattern(const KeyPattern& keyPattern) {
    invariant(!keyPattern.toBSON().isEmpty());
    _keyPattern = keyPattern;
}

bool ShardCollectionType::getRefreshing() const {
    invariant(_refreshing);
    return _refreshing.get();
}

const ChunkVersion& ShardCollectionType::getLastRefreshedCollectionVersion() const {
    invariant(_lastRefreshedCollectionVersion);
    return _lastRefreshedCollectionVersion.get();
}

}  // namespace mongo
