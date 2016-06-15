/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/s/move_chunk_request.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"

namespace mongo {
namespace {

const char kMoveChunk[] = "moveChunk";
const char kConfigServerConnectionString[] = "configdb";
const char kFromShardId[] = "fromShard";
const char kToShardId[] = "toShard";
const char kMaxChunkSizeBytes[] = "maxChunkSizeBytes";
const char kWaitForDelete[] = "waitForDelete";
const char kTakeDistLock[] = "takeDistLock";

}  // namespace

MoveChunkRequest::MoveChunkRequest(NamespaceString nss,
                                   ChunkRange range,
                                   MigrationSecondaryThrottleOptions secondaryThrottle)
    : _nss(std::move(nss)),
      _range(std::move(range)),
      _secondaryThrottle(std::move(secondaryThrottle)) {}

StatusWith<MoveChunkRequest> MoveChunkRequest::createFromCommand(NamespaceString nss,
                                                                 const BSONObj& obj) {
    auto secondaryThrottleStatus = MigrationSecondaryThrottleOptions::createFromCommand(obj);
    if (!secondaryThrottleStatus.isOK()) {
        return secondaryThrottleStatus.getStatus();
    }

    auto rangeStatus = ChunkRange::fromBSON(obj);
    if (!rangeStatus.isOK()) {
        return rangeStatus.getStatus();
    }

    MoveChunkRequest request(std::move(nss),
                             std::move(rangeStatus.getValue()),
                             std::move(secondaryThrottleStatus.getValue()));

    {
        std::string configServerConnectionString;
        Status status = bsonExtractStringField(
            obj, kConfigServerConnectionString, &configServerConnectionString);
        if (!status.isOK()) {
            return status;
        }

        auto statusConfigServerCS = ConnectionString::parse(configServerConnectionString);
        if (!statusConfigServerCS.isOK()) {
            return statusConfigServerCS.getStatus();
        }

        request._configServerCS = std::move(statusConfigServerCS.getValue());
    }

    {
        std::string shardStr;
        Status status = bsonExtractStringField(obj, kFromShardId, &shardStr);
        request._fromShardId = shardStr;
        if (!status.isOK()) {
            return status;
        }
    }

    {
        std::string shardStr;
        Status status = bsonExtractStringField(obj, kToShardId, &shardStr);
        request._toShardId = shardStr;
        if (!status.isOK()) {
            return status;
        }
    }

    {
        Status status =
            bsonExtractBooleanFieldWithDefault(obj, kWaitForDelete, false, &request._waitForDelete);
        if (!status.isOK()) {
            return status;
        }
    }

    {
        long long maxChunkSizeBytes;
        Status status = bsonExtractIntegerField(obj, kMaxChunkSizeBytes, &maxChunkSizeBytes);
        if (!status.isOK()) {
            return status;
        }

        request._maxChunkSizeBytes = static_cast<int64_t>(maxChunkSizeBytes);
    }

    {
        Status status =
            bsonExtractBooleanFieldWithDefault(obj, kTakeDistLock, true, &request._takeDistLock);
        if (!status.isOK()) {
            return status;
        }
    }

    return request;
}

void MoveChunkRequest::appendAsCommand(BSONObjBuilder* builder,
                                       const NamespaceString& nss,
                                       const ChunkVersion& shardVersion,
                                       const ConnectionString& configServerConnectionString,
                                       const ShardId& fromShardId,
                                       const ShardId& toShardId,
                                       const ChunkRange& range,
                                       int64_t maxChunkSizeBytes,
                                       const MigrationSecondaryThrottleOptions& secondaryThrottle,
                                       bool waitForDelete,
                                       bool takeDistLock) {
    invariant(builder->asTempObj().isEmpty());
    invariant(nss.isValid());

    builder->append(kMoveChunk, nss.ns());
    shardVersion.appendForCommands(builder);
    builder->append(kConfigServerConnectionString, configServerConnectionString.toString());
    builder->append(kFromShardId, fromShardId.toString());
    builder->append(kToShardId, toShardId.toString());
    range.append(builder);
    builder->append(kMaxChunkSizeBytes, static_cast<long long>(maxChunkSizeBytes));
    secondaryThrottle.append(builder);
    builder->append(kWaitForDelete, waitForDelete);
    builder->append(kTakeDistLock, takeDistLock);
}

bool MoveChunkRequest::operator==(const MoveChunkRequest& other) const {
    if (_nss != other._nss)
        return false;
    if (_configServerCS != other._configServerCS)
        return false;
    if (_fromShardId != other._fromShardId)
        return false;
    if (_toShardId != other._toShardId)
        return false;
    if (_range != other._range)
        return false;
    if (_maxChunkSizeBytes != other._maxChunkSizeBytes)
        return false;
    if (_secondaryThrottle != other._secondaryThrottle)
        return false;
    if (_waitForDelete != other._waitForDelete)
        return false;
    if (_takeDistLock != other._takeDistLock)
        return false;

    return true;
}

bool MoveChunkRequest::operator!=(const MoveChunkRequest& other) const {
    return !(*this == other);
}

}  // namespace mongo
