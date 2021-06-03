/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/s/request_types/merge_chunks_request_type.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/platform/basic.h"

namespace mongo {
namespace {

const char kConfigsvrMergeChunks[] = "_configsvrCommitChunksMerge";
const char kCollUUID[] = "collUUID";
const char kChunkRange[] = "chunkRange";
const char kShardId[] = "shard";
const char kValidAfter[] = "validAfter";
}  // namespace

MergeChunksRequest::MergeChunksRequest(NamespaceString nss,
                                       ShardId shardId,
                                       UUID collectionUUID,
                                       ChunkRange chunkRange,
                                       boost::optional<Timestamp> validAfter)
    : _nss(std::move(nss)),
      _collectionUUID(std::move(collectionUUID)),
      _chunkRange(std::move(chunkRange)),
      _shardId(std::move(shardId)),
      _validAfter(validAfter) {}

StatusWith<MergeChunksRequest> MergeChunksRequest::parseFromConfigCommand(const BSONObj& cmdObj) {
    std::string ns;
    {
        auto parseNamespaceStatus = bsonExtractStringField(cmdObj, kConfigsvrMergeChunks, &ns);
        if (!parseNamespaceStatus.isOK()) {
            return parseNamespaceStatus;
        }
    }
    NamespaceString nss(ns);
    if (!nss.isValid()) {
        return {ErrorCodes::InvalidNamespace,
                str::stream() << "invalid namespace '" << nss.ns() << "' specified for request"};
    }

    BSONElement collUUIDElem;
    {
        auto parseCollUUIDStatus =
            bsonExtractTypedField(cmdObj, kCollUUID, mongo::Object, &collUUIDElem);
        if (!parseCollUUIDStatus.isOK()) {
            return parseCollUUIDStatus;
        }
    }

    auto collUUID = UUID::parse(collUUIDElem.Obj().getField("uuid"));
    if (!collUUID.isOK()) {
        return collUUID.getStatus();
    }

    BSONElement chunkRangeElem;
    {
        auto chunkRangeStatus =
            bsonExtractTypedField(cmdObj, kChunkRange, mongo::Object, &chunkRangeElem);
        if (!chunkRangeStatus.isOK()) {
            return chunkRangeStatus;
        }
    }

    auto chunkRange = ChunkRange::fromBSON(chunkRangeElem.Obj().getOwned());
    if (!chunkRange.isOK()) {
        return chunkRange.getStatus();
    }

    std::string shardIdString;
    {
        auto parseShardIdStatus = bsonExtractStringField(cmdObj, kShardId, &shardIdString);
        if (!parseShardIdStatus.isOK()) {
            return parseShardIdStatus;
        }
    }

    boost::optional<Timestamp> validAfter = boost::none;
    {
        Timestamp ts{0};
        auto status = bsonExtractTimestampField(cmdObj, kValidAfter, &ts);
        if (!status.isOK() && status != ErrorCodes::NoSuchKey) {
            return status;
        }

        if (status.isOK()) {
            validAfter = ts;
        }
    }

    return MergeChunksRequest(std::move(nss),
                              ShardId(shardIdString),
                              std::move(collUUID.getValue()),
                              std::move(chunkRange.getValue()),
                              validAfter);
}

BSONObj MergeChunksRequest::toConfigCommandBSON(const BSONObj& writeConcern) {
    BSONObjBuilder cmdBuilder;
    appendAsConfigCommand(&cmdBuilder);

    // Tack on passed-in writeConcern
    cmdBuilder.append(WriteConcernOptions::kWriteConcernField, writeConcern);

    return cmdBuilder.obj();
}

void MergeChunksRequest::appendAsConfigCommand(BSONObjBuilder* cmdBuilder) {
    cmdBuilder->append(kConfigsvrMergeChunks, _nss.ns());
    cmdBuilder->append(kCollUUID, _collectionUUID.toBSON());
    cmdBuilder->append(kChunkRange, _chunkRange.toBSON());
    cmdBuilder->append(kShardId, _shardId);
    invariant(_validAfter.is_initialized());
    cmdBuilder->append(kValidAfter, _validAfter.get());
}


}  // namespace mongo
