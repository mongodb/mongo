// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/type_chunk.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/sharding_environment/shard_ref.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstring>
#include <string_view>

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
Status extractObject(const BSONObj& obj, std::string_view fieldName, BSONElement* bsonElement) {
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

ChunkType::ChunkType(UUID collectionUUID, ChunkRange range, ChunkVersion version, ShardRef shard)
    : _collectionUUID(collectionUUID),
      _range(range),
      _version(std::move(version)),
      _shard(std::move(shard)) {}

StatusWith<ChunkType> ChunkType::_parseChunkBase(const BSONObj& source) {
    ChunkType chunk;

    {
        BSONElement shardElem = source[shard.name()];
        if (shardElem.eoo()) {
            return {ErrorCodes::NoSuchKey,
                    str::stream() << "The field '" << shard.name() << "' is missing"};
        }
        try {
            chunk._shard = ShardRef::parse(shardElem);
        } catch (const DBException& e) {
            return e.toStatus().withContext(str::stream() << "The field '" << shard.name()
                                                          << "' cannot be parsed");
        }
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

std::vector<ChunkType> ChunkType::parseConfigBSONDocuments(const std::vector<BSONObj>& chunkDocs,
                                                           const UUID& expectedCollectionUUID,
                                                           const OID& epoch,
                                                           const Timestamp& timestamp) {
    std::vector<ChunkType> chunks;
    chunks.reserve(chunkDocs.size());

    for (const auto& chunkDoc : chunkDocs) {
        auto chunk = uassertStatusOK(parseFromConfigBSON(chunkDoc, epoch, timestamp));
        uassert(12698702,
                str::stream() << "Chunk " << chunk.toString()
                              << " does not belong to collection UUID " << expectedCollectionUUID,
                chunk.getCollectionUUID() == expectedCollectionUUID);
        uassertStatusOK(chunk.validate());
        chunks.push_back(std::move(chunk));
    }

    return chunks;
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

StatusWith<ChunkType> ChunkType::parseFromNetworkRequest(const BSONObj& source,
                                                         bool acceptMissingVersion) {
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
    {
        const auto elem = source[ChunkType::lastmod()];
        if (!acceptMissingVersion || !elem.eoo()) {
            chunk._version = ChunkVersion::parse(elem);
        }
    }

    return chunk;
}

BSONObj ChunkType::toConfigBSON(bool omitVersion) const {
    BSONObjBuilder builder;
    if (_id)
        builder.append(name.name(), getName());
    if (_collectionUUID)
        _collectionUUID->appendToBuilder(&builder, collectionUUID.name());
    if (_range)
        _range->serialize(&builder);
    if (_shard)
        getShard().serialize(shard.name(), &builder);
    if (!omitVersion && _version)
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
    getShard().serialize(shard.name(), &builder);
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

void ChunkType::setShard(const ShardRef& shard) {
    invariant(ShardRef::validate(shard));
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

    if (!_shard.has_value() || !ShardRef::validate(*_shard).isOK()) {
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
