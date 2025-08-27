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

#include "mongo/db/global_catalog/type_chunk.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstring>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

// The final namespace of the cached chunks metadata is composed of the namespace of the related
// sharded collection (i.e., config.cache.chunks.<ns>). As a result, the maximum namespace length of
// sharded collections is reduced. See NamespaceString::MaxUserNsShardedCollectionLen.
const std::string ChunkType::ShardNSPrefix = "config.cache.chunks.";

const BSONField<OID> ChunkType::name("_id");
const BSONField<BSONObj> ChunkType::minShardID("_id");
const BSONField<UUID> ChunkType::collectionUUID("uuid");
const BSONField<BSONObj> ChunkType::min(std::string{ChunkRange::kMinFieldName});
const BSONField<BSONObj> ChunkType::max(std::string{ChunkRange::kMaxFieldName});
const BSONField<std::string> ChunkType::shard("shard");
const BSONField<bool> ChunkType::jumbo("jumbo");
const BSONField<Date_t> ChunkType::lastmod("lastmod");
const BSONField<BSONObj> ChunkType::history("history");
const BSONField<int64_t> ChunkType::estimatedSizeBytes("estimatedDataSizeBytes");
const BSONField<Timestamp> ChunkType::onCurrentShardSince("onCurrentShardSince");
const BSONField<bool> ChunkType::historyIsAt40("historyIsAt40");

namespace {

/**
 * Extracts an Object value from 'obj's field 'fieldName'. Sets the result to 'bsonElement'.
 */
Status extractObject(const BSONObj& obj, StringData fieldName, BSONElement* bsonElement) {
    Status elementStatus = bsonExtractTypedField(obj, fieldName, BSONType::object, bsonElement);
    if (!elementStatus.isOK()) {
        return elementStatus.withContext(str::stream()
                                         << "The field '" << fieldName << "' cannot be parsed");
    }

    if (bsonElement->Obj().isEmpty()) {
        return {ErrorCodes::BadValue,
                str::stream() << "The field '" << fieldName << "' cannot be empty"};
    }

    return Status::OK();
}

}  // namespace

StatusWith<std::vector<ChunkHistory>> ChunkHistory::fromBSON(const BSONArray& source) {
    std::vector<ChunkHistory> values;

    for (const auto& arrayElement : source) {
        if (arrayElement.type() == BSONType::object) {
            IDLParserContext tempContext("chunk history array");
            values.emplace_back(ChunkHistoryBase::parse(arrayElement.Obj(), tempContext));
        } else {
            return {ErrorCodes::BadValue,
                    str::stream() << "array element does not have the object type: "
                                  << arrayElement.type()};
        }
    }

    return values;
}

// ChunkType

ChunkType::ChunkType() = default;

ChunkType::ChunkType(UUID collectionUUID, ChunkRange range, ChunkVersion version, ShardId shardId)
    : _collectionUUID(collectionUUID),
      _range(range),
      _version(std::move(version)),
      _shard(std::move(shardId)) {}

StatusWith<ChunkType> ChunkType::_parseChunkBase(const BSONObj& source) {
    ChunkType chunk;

    {
        std::string chunkShard;
        Status status = bsonExtractStringField(source, shard.name(), &chunkShard);
        if (!status.isOK())
            return status;
        chunk._shard = chunkShard;
    }

    {
        BSONElement historyObj;
        Status status = bsonExtractTypedField(source, history.name(), BSONType::array, &historyObj);
        if (status.isOK()) {
            auto history = ChunkHistory::fromBSON(BSONArray(historyObj.Obj()));
            if (!history.isOK())
                return history.getStatus();

            chunk._history = std::move(history.getValue());

        } else if (status == ErrorCodes::NoSuchKey) {
            // History is missing, so it will be presumed empty
        } else {
            return status;
        }
    }

    {
        if (!chunk._history.empty()) {
            Timestamp onCurrentShardSinceValue;
            Status status = bsonExtractTimestampField(
                source, onCurrentShardSince.name(), &onCurrentShardSinceValue);
            if (status.isOK()) {
                chunk._onCurrentShardSince = onCurrentShardSinceValue;
                if (chunk._history.front().getValidAfter() != onCurrentShardSinceValue) {
                    return {ErrorCodes::BadValue,
                            str::stream()
                                << "The first `validAfter` in the chunk's history is not "
                                   "consistent with `onCurrentShardSince`: validAfter is "
                                << chunk._history.front().getValidAfter()
                                << " while onCurrentShardSince is " << *chunk._onCurrentShardSince};
                }
            } else {
                chunk._onCurrentShardSince = boost::none;
            }
        }
    }

    return chunk;
}

StatusWith<ChunkType> ChunkType::parseFromConfigBSON(const BSONObj& source,
                                                     const OID& epoch,
                                                     const Timestamp& timestamp) {
    // Parse shard, and history
    StatusWith<ChunkType> chunkStatus = _parseChunkBase(source);
    if (!chunkStatus.isOK()) {
        return chunkStatus.getStatus();
    }
    ChunkType chunk = chunkStatus.getValue();

    // Parse collectionUUID.
    {
        BSONElement collectionUUIDElem;
        Status status = bsonExtractField(source, collectionUUID.name(), &collectionUUIDElem);
        if (status.isOK()) {
            auto swUUID = UUID::parse(collectionUUIDElem);
            if (!swUUID.isOK()) {
                return swUUID.getStatus();
            }
            chunk._collectionUUID = uassertStatusOK(UUID::parse(collectionUUIDElem));
        } else {
            return status;
        }
    }

    // Parse id.
    {
        OID chunkID;
        Status status = bsonExtractOIDField(source, name.name(), &chunkID);
        if (status.isOK()) {
            chunk._id = chunkID;
        } else {
            return status;
        }
    }

    // Parse lastmod.
    {
        auto versionElem = source[ChunkType::lastmod()];
        if (versionElem.eoo())
            return Status(ErrorCodes::NoSuchKey, "No version found");
        if (versionElem.type() == BSONType::timestamp || versionElem.type() == BSONType::date) {
            auto chunkLastmod = Timestamp(versionElem._numberLong());
            chunk._version =
                ChunkVersion({epoch, timestamp}, {chunkLastmod.getSecs(), chunkLastmod.getInc()});
        } else {
            return {ErrorCodes::BadValue,
                    str::stream() << "The field " << ChunkType::lastmod() << " cannot be parsed."};
        }
    }

    // Parse range.
    {
        try {
            chunk._range = ChunkRange::fromBSON(source);
        } catch (const DBException& e) {
            return e.toStatus().withContext("Failed to parse chunk range");
        }
    }

    // Parse estimatedSizeBytes if present.
    {
        auto elem = source.getField(estimatedSizeBytes.name());
        if (!elem.eoo()) {
            chunk._estimatedSizeBytes = elem.safeNumberLong();
        }
    }

    // Parse jumbo flag.
    {
        bool chunkJumbo;
        Status status = bsonExtractBooleanField(source, jumbo.name(), &chunkJumbo);
        if (status.isOK()) {
            chunk._jumbo = chunkJumbo;
        } else if (status == ErrorCodes::NoSuchKey) {
            // Jumbo status is missing, so it will be presumed false
        } else {
            return status;
        }
    }

    return chunk;
}

StatusWith<ChunkType> ChunkType::parseFromShardBSON(const BSONObj& source,
                                                    const OID& epoch,
                                                    const Timestamp& timestamp) {
    // Parse history and shard.
    StatusWith<ChunkType> chunkStatus = _parseChunkBase(source);
    if (!chunkStatus.isOK()) {
        return chunkStatus.getStatus();
    }

    ChunkType chunk = chunkStatus.getValue();

    // Parse min and max.
    {
        BSONElement minKey;
        Status minKeyStatus = extractObject(source, minShardID.name(), &minKey);
        if (!minKeyStatus.isOK()) {
            return minKeyStatus;
        }

        BSONElement maxKey;
        Status maxKeyStatus = extractObject(source, ChunkRange::kMaxFieldName, &maxKey);
        if (!maxKeyStatus.isOK()) {
            return maxKeyStatus;
        }

        auto range = ChunkRange(minKey.Obj().getOwned(), maxKey.Obj().getOwned());
        auto rangeValidateStatus = ChunkRange::validate(range.getMin(), range.getMax());
        if (!rangeValidateStatus.isOK()) {
            return rangeValidateStatus;
        }
        chunk._range = boost::make_optional<ChunkRange>(std::move(range));
    }

    // Parse version.
    {
        auto lastmodElem = source[ChunkType::lastmod()];
        if (lastmodElem.eoo())
            return Status(ErrorCodes::NoSuchKey, "No version found");
        if (lastmodElem.type() == BSONType::timestamp || lastmodElem.type() == BSONType::date) {
            auto chunkLastmod = Timestamp(lastmodElem._numberLong());
            chunk._version =
                ChunkVersion({epoch, timestamp}, {chunkLastmod.getSecs(), chunkLastmod.getInc()});
        } else {
            return {ErrorCodes::NoSuchKey,
                    str::stream() << "Expected field " << ChunkType::lastmod() << " not found."};
        }
    }

    return chunk;
}

StatusWith<ChunkType> ChunkType::parseFromNetworkRequest(const BSONObj& source) {
    // Parse history and shard.
    StatusWith<ChunkType> chunkStatus = _parseChunkBase(source);
    if (!chunkStatus.isOK()) {
        return chunkStatus.getStatus();
    }

    ChunkType chunk = chunkStatus.getValue();

    // Parse UUID.
    {
        BSONElement collectionUUIDElem;
        Status status = bsonExtractField(source, collectionUUID.name(), &collectionUUIDElem);
        if (status.isOK()) {
            auto swUUID = UUID::parse(collectionUUIDElem);
            if (!swUUID.isOK()) {
                return swUUID.getStatus();
            }
            chunk._collectionUUID = swUUID.getValue();
        } else if (status == ErrorCodes::NoSuchKey) {
            return {ErrorCodes::FailedToParse, str::stream() << "There must be a UUID present"};
        } else {
            return status;
        }
    }

    // Parse min and max.
    {
        try {
            chunk._range = ChunkRange::fromBSON(source);
        } catch (const DBException& e) {
            return e.toStatus().withContext("Failed to parse chunk range");
        }
    }

    // Parse jumbo.
    {
        bool chunkJumbo;
        Status status = bsonExtractBooleanField(source, jumbo.name(), &chunkJumbo);
        if (status.isOK()) {
            chunk._jumbo = chunkJumbo;
        } else if (status == ErrorCodes::NoSuchKey) {
            // Jumbo status is missing, so it will be presumed false
        } else {
            return status;
        }
    }

    // Parse version.
    chunk._version = ChunkVersion::parse(source[ChunkType::lastmod()]);

    return chunk;
}

BSONObj ChunkType::toConfigBSON() const {
    BSONObjBuilder builder;
    if (_id)
        builder.append(name.name(), getName());
    if (_collectionUUID)
        _collectionUUID->appendToBuilder(&builder, collectionUUID.name());
    if (_range)
        _range->serialize(&builder);
    if (_shard)
        builder.append(shard.name(), getShard().toString());
    if (_version)
        builder.appendTimestamp(lastmod.name(), _version->toLong());
    if (_estimatedSizeBytes)
        builder.appendNumber(estimatedSizeBytes.name(),
                             static_cast<long long>(*_estimatedSizeBytes));
    if (_jumbo)
        builder.append(jumbo.name(), getJumbo());
    addHistoryToBSON(builder);
    return builder.obj();
}

BSONObj ChunkType::toShardBSON() const {
    BSONObjBuilder builder;
    invariant(_range);
    invariant(_shard);
    invariant(_version);
    builder.append(minShardID.name(), _range->getMin());
    builder.append(ChunkRange::kMaxFieldName, _range->getMax());
    builder.append(shard.name(), getShard().toString());
    builder.appendTimestamp(lastmod.name(), _version->toLong());
    addHistoryToBSON(builder);
    return builder.obj();
}

const OID& ChunkType::getName() const {
    uassert(51264, "Chunk name is not set", _id);
    return *_id;
}

void ChunkType::setName(const OID& id) {
    _id = id;
}

void ChunkType::setCollectionUUID(const UUID& uuid) {
    _collectionUUID = uuid;
}

void ChunkType::setRange(const ChunkRange& range) {
    _range = range;
}

void ChunkType::setVersion(const ChunkVersion& version) {
    invariant(version.isSet());
    _version = version;
}

void ChunkType::setShard(const ShardId& shard) {
    invariant(shard.isValid());
    _shard = shard;
}

void ChunkType::setEstimatedSizeBytes(const boost::optional<int64_t>& estimatedSize) {
    uassert(ErrorCodes::BadValue,
            "estimatedSizeBytes cannot be negative",
            !estimatedSize.has_value() || estimatedSize.value() >= 0);
    _estimatedSizeBytes = estimatedSize;
}

void ChunkType::setJumbo(bool jumbo) {
    _jumbo = jumbo;
}

void ChunkType::setOnCurrentShardSince(const Timestamp& onCurrentShardSince) {
    _onCurrentShardSince = onCurrentShardSince;
}

void ChunkType::addHistoryToBSON(BSONObjBuilder& builder) const {
    if (_history.size()) {
        if (_onCurrentShardSince.has_value()) {
            uassert(ErrorCodes::BadValue,
                    str::stream() << "The first `validAfter` in the chunk's history is not "
                                     "consistent with `onCurrentShardSince`: validAfter is "
                                  << _history.front().getValidAfter()
                                  << " while onCurrentShardSince is " << *_onCurrentShardSince,
                    _history.front().getValidAfter() == *_onCurrentShardSince);
            builder.append(onCurrentShardSince.name(), *_onCurrentShardSince);
        }
        {
            BSONArrayBuilder arrayBuilder(builder.subarrayStart(history.name()));
            for (const auto& item : _history) {
                BSONObjBuilder subObjBuilder(arrayBuilder.subobjStart());
                item.serialize(&subObjBuilder);
            }
        }
    }
}

Status ChunkType::validate() const {
    if (!_version.has_value() || !_version->isSet()) {
        return Status(ErrorCodes::NoSuchKey, str::stream() << "missing version field");
    }

    if (!_shard.has_value() || !_shard->isValid()) {
        return Status(ErrorCodes::NoSuchKey,
                      str::stream() << "missing " << shard.name() << " field");
    }

    if (!_range.has_value()) {
        return Status(ErrorCodes::NoSuchKey, str::stream() << "missing range field");
    }

    auto rangeValidationStatus = ChunkRange::validateStrict(*_range);
    if (!rangeValidationStatus.isOK()) {
        return rangeValidationStatus;
    }

    if (!_history.empty()) {
        if (_history.front().getShard() != *_shard) {
            return {ErrorCodes::BadValue,
                    str::stream() << "Latest entry of chunk history refer to shard "
                                  << _history.front().getShard()
                                  << " that does not match the current shard " << *_shard};
        }
        if (_onCurrentShardSince.has_value() &&
            _history.front().getValidAfter() != *_onCurrentShardSince) {
            return {ErrorCodes::BadValue,
                    str::stream() << "The first `validAfter` in the chunk's `history` is not "
                                     "consistent with `onCurrentShardSince`: validAfter is "
                                  << _history.front().getValidAfter()
                                  << " while onCurrentShardSince is " << *_onCurrentShardSince};
        }
    }

    return Status::OK();
}

std::string ChunkType::toString() const {
    // toConfigBSON will include all the set fields, whereas toShardBSON includes only a subset and
    // requires them to be set.
    return toConfigBSON().toString();
}

}  // namespace mongo
