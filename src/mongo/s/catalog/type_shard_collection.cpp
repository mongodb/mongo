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

const std::string ShardCollectionType::ConfigNS = "config.collections";

const BSONField<std::string> ShardCollectionType::uuid("_id");
const BSONField<std::string> ShardCollectionType::ns("ns");
const BSONField<BSONObj> ShardCollectionType::keyPattern("key");
const BSONField<OID> ShardCollectionType::lastConsistentCollectionVersionEpoch(
    "lastConsistentCollectionVersionEpoch");
const BSONField<Date_t> ShardCollectionType::lastConsistentCollectionVersion(
    "lastConsistentCollectionVersion");

ShardCollectionType::ShardCollectionType(const CollectionType& collectionType)
    : ShardCollectionType(
          collectionType.getNs(), collectionType.getNs(), collectionType.getKeyPattern()) {}

ShardCollectionType::ShardCollectionType(const NamespaceString& uuid,
                                         const NamespaceString& ns,
                                         const KeyPattern& keyPattern)
    : _uuid(uuid), _ns(ns), _keyPattern(keyPattern.toBSON()) {}

StatusWith<ShardCollectionType> ShardCollectionType::fromBSON(const BSONObj& source) {
    NamespaceString uuidNss;
    {
        std::string uuidString;
        Status status = bsonExtractStringField(source, uuid.name(), &uuidString);
        if (!status.isOK()) {
            return status;
        }
        uuidNss = NamespaceString{uuidString};
    }

    NamespaceString nsNss;
    {
        std::string nsString;
        Status status = bsonExtractStringField(source, ns.name(), &nsString);
        if (!status.isOK()) {
            return status;
        }
        nsNss = NamespaceString{nsString};
    }

    BSONElement collKeyPattern;
    Status status = bsonExtractTypedField(source, keyPattern.name(), Object, &collKeyPattern);
    if (!status.isOK()) {
        return status;
    }
    BSONObj obj = collKeyPattern.Obj();
    if (obj.isEmpty()) {
        return Status(ErrorCodes::ShardKeyNotFound,
                      str::stream() << "Empty shard key. Failed to parse: " << source.toString());
    }
    KeyPattern pattern(obj.getOwned());

    ShardCollectionType shardCollectionType(uuidNss, nsNss, pattern);

    {
        auto statusWithChunkVersion = ChunkVersion::parseFromBSONWithFieldForCommands(
            source, lastConsistentCollectionVersion.name());
        if (statusWithChunkVersion.isOK()) {
            ChunkVersion collVersion = std::move(statusWithChunkVersion.getValue());
            shardCollectionType.setLastConsistentCollectionVersion(std::move(collVersion));
        } else if (statusWithChunkVersion == ErrorCodes::NoSuchKey) {
            // May not be set yet, which is okay.
        } else {
            return statusWithChunkVersion.getStatus();
        }
    }

    return shardCollectionType;
}

bool ShardCollectionType::isLastConsistentCollectionVersionSet() const {
    return _lastConsistentCollectionVersion.is_initialized();
}

BSONObj ShardCollectionType::toBSON() const {
    BSONObjBuilder builder;

    builder.append(uuid.name(), _uuid.toString());
    builder.append(ns.name(), _ns.toString());
    builder.append(keyPattern.name(), _keyPattern.toBSON());

    if (_lastConsistentCollectionVersion) {
        _lastConsistentCollectionVersion->appendWithFieldForCommands(
            &builder, lastConsistentCollectionVersion.name());
    }

    return builder.obj();
}

std::string ShardCollectionType::toString() const {
    return toBSON().toString();
}

void ShardCollectionType::setUUID(const NamespaceString& uuid) {
    invariant(uuid.isValid());
    _uuid = uuid;
}

void ShardCollectionType::setNs(const NamespaceString& ns) {
    invariant(ns.isValid());
    _ns = ns;
}

void ShardCollectionType::setKeyPattern(const KeyPattern& keyPattern) {
    invariant(!keyPattern.toBSON().isEmpty());
    _keyPattern = keyPattern;
}

void ShardCollectionType::setLastConsistentCollectionVersion(
    const ChunkVersion& lastConsistentCollectionVersion) {
    _lastConsistentCollectionVersion = lastConsistentCollectionVersion;
}

const OID ShardCollectionType::getLastConsistentCollectionVersionEpoch() const {
    invariant(_lastConsistentCollectionVersion);
    return _lastConsistentCollectionVersion->epoch();
}

}  // namespace mongo
