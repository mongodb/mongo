// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/split_chunk_request_type.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/str.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

const char kConfigsvrSplitChunk[] = "_configsvrCommitChunkSplit";
const char kCollEpoch[] = "collEpoch";
const char kCollTimestamp[] = "collTimestamp";
const char kSplitPoints[] = "splitPoints";
const char kShardName[] = "shard";

}  // namespace

SplitChunkRequest::SplitChunkRequest(NamespaceString nss,
                                     std::string shardName,
                                     OID epoch,
                                     boost::optional<Timestamp> timestamp,
                                     ChunkRange chunkRange,
                                     std::vector<BSONObj> splitPoints)
    : _nss(std::move(nss)),
      _epoch(std::move(epoch)),
      _timestamp(std::move(timestamp)),
      _chunkRange(std::move(chunkRange)),
      _splitPoints(std::move(splitPoints)),
      _shardName(std::move(shardName)) {}

StatusWith<SplitChunkRequest> SplitChunkRequest::parseFromConfigCommand(const BSONObj& cmdObj) {
    std::string ns;
    auto parseNamespaceStatus = bsonExtractStringField(cmdObj, kConfigsvrSplitChunk, &ns);

    if (!parseNamespaceStatus.isOK()) {
        return parseNamespaceStatus;
    }

    OID epoch;
    auto parseEpochStatus = bsonExtractOIDField(cmdObj, kCollEpoch, &epoch);

    if (!parseEpochStatus.isOK()) {
        return parseEpochStatus;
    }

    boost::optional<Timestamp> timestamp;
    if (cmdObj[kCollTimestamp]) {
        timestamp.emplace();
        auto parseTimestampStatus =
            bsonExtractTimestampField(cmdObj, kCollTimestamp, timestamp.get_ptr());

        if (!parseTimestampStatus.isOK()) {
            return parseTimestampStatus;
        }
    }


    auto chunkRangeStatus = [&]() -> StatusWith<ChunkRange> {
        try {
            return ChunkRange::fromBSON(cmdObj);
        } catch (const DBException& e) {
            return e.toStatus().withContext("Failed to parse chunk range");
        }
    }();

    if (!chunkRangeStatus.isOK()) {
        return chunkRangeStatus.getStatus();
    }

    std::vector<BSONObj> splitPoints;
    {
        BSONElement splitPointsElem;
        auto splitPointsElemStatus =
            bsonExtractTypedField(cmdObj, kSplitPoints, BSONType::array, &splitPointsElem);

        if (!splitPointsElemStatus.isOK()) {
            return splitPointsElemStatus;
        }
        BSONObjIterator it(splitPointsElem.Obj());
        while (it.more()) {
            splitPoints.push_back(it.next().Obj().getOwned());
        }
    }

    std::string shardName;
    auto parseShardNameStatus = bsonExtractStringField(cmdObj, kShardName, &shardName);

    if (!parseShardNameStatus.isOK()) {
        return parseShardNameStatus;
    }

    auto request = SplitChunkRequest(
        NamespaceStringUtil::deserialize(boost::none, ns, SerializationContext::stateDefault()),
        std::move(shardName),
        std::move(epoch),
        std::move(timestamp),
        std::move(chunkRangeStatus.getValue()),
        std::move(splitPoints));
    Status validationStatus = request._validate();
    if (!validationStatus.isOK()) {
        return validationStatus;
    }

    return request;
}

BSONObj SplitChunkRequest::toConfigCommandBSON(const BSONObj& writeConcern) {
    BSONObjBuilder cmdBuilder;
    appendAsConfigCommand(&cmdBuilder);

    // Tack on passed-in writeConcern
    cmdBuilder.append(WriteConcernOptions::kWriteConcernField, writeConcern);

    return cmdBuilder.obj();
}

void SplitChunkRequest::appendAsConfigCommand(BSONObjBuilder* cmdBuilder) {
    cmdBuilder->append(kConfigsvrSplitChunk,
                       NamespaceStringUtil::serialize(_nss, SerializationContext::stateDefault()));
    cmdBuilder->append(kCollEpoch, _epoch);
    _chunkRange.serialize(cmdBuilder);
    {
        BSONArrayBuilder splitPointsArray(cmdBuilder->subarrayStart(kSplitPoints));
        for (const auto& splitPoint : _splitPoints) {
            splitPointsArray.append(splitPoint);
        }
    }
    cmdBuilder->append(kShardName, _shardName);
}

const NamespaceString& SplitChunkRequest::getNamespace() const {
    return _nss;
}

const OID& SplitChunkRequest::getEpoch() const {
    return _epoch;
}

const ChunkRange& SplitChunkRequest::getChunkRange() const {
    return _chunkRange;
}

const std::vector<BSONObj>& SplitChunkRequest::getSplitPoints() const {
    return _splitPoints;
}

const std::string& SplitChunkRequest::getShardName() const {
    return _shardName;
}

Status SplitChunkRequest::_validate() {
    if (!getNamespace().isValid()) {
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid namespace '" << _nss.toStringForErrorMsg()
                                    << "' specified for request");
    }

    if (getSplitPoints().empty()) {
        return Status(ErrorCodes::InvalidOptions, "need to provide the split points");
    }

    return Status::OK();
}

}  // namespace mongo
