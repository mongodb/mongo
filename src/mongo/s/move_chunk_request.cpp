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
const char kChunkMinKey[] = "min";
const char kChunkMaxKey[] = "max";
const char kMaxChunkSizeBytes[] = "maxChunkSizeBytes";
const char kWaitForDelete[] = "waitForDelete";

}  // namespace

MoveChunkRequest::MoveChunkRequest(NamespaceString nss,
                                   MigrationSecondaryThrottleOptions secondaryThrottle)
    : _nss(std::move(nss)), _secondaryThrottle(std::move(secondaryThrottle)) {}

StatusWith<MoveChunkRequest> MoveChunkRequest::createFromCommand(NamespaceString nss,
                                                                 const BSONObj& obj) {
    auto secondaryThrottleStatus = MigrationSecondaryThrottleOptions::createFromCommand(obj);
    if (!secondaryThrottleStatus.isOK()) {
        return secondaryThrottleStatus.getStatus();
    }

    MoveChunkRequest request(std::move(nss), std::move(secondaryThrottleStatus.getValue()));

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
        Status status = bsonExtractStringField(obj, kFromShardId, &request._fromShardId);
        if (!status.isOK()) {
            return status;
        }
    }

    {
        Status status = bsonExtractStringField(obj, kToShardId, &request._toShardId);
        if (!status.isOK()) {
            return status;
        }
    }

    {
        BSONElement elem;
        Status status = bsonExtractTypedField(obj, kChunkMinKey, BSONType::Object, &elem);
        if (!status.isOK()) {
            return status;
        }

        request._minKey = elem.Obj().getOwned();

        if (request._minKey.isEmpty()) {
            return Status(ErrorCodes::UnsupportedFormat, "The chunk min key cannot be empty");
        }
    }

    {
        BSONElement elem;
        Status status = bsonExtractTypedField(obj, kChunkMaxKey, BSONType::Object, &elem);
        if (!status.isOK()) {
            return status;
        }

        request._maxKey = elem.Obj().getOwned();

        if (request._maxKey.isEmpty()) {
            return Status(ErrorCodes::UnsupportedFormat, "The chunk max key cannot be empty");
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

    return request;
}

void MoveChunkRequest::appendAsCommand(BSONObjBuilder* builder,
                                       const NamespaceString& nss,
                                       const ChunkVersion& shardVersion,
                                       const ConnectionString& configServerConnectionString,
                                       const std::string& fromShardId,
                                       const std::string& toShardId,
                                       const BSONObj& chunkMinKey,
                                       const BSONObj& chunkMaxKey,
                                       int64_t maxChunkSizeBytes,
                                       const MigrationSecondaryThrottleOptions& secondaryThrottle,
                                       bool waitForDelete) {
    invariant(builder->asTempObj().isEmpty());
    invariant(nss.isValid());

    builder->append(kMoveChunk, nss.ns());
    shardVersion.appendForCommands(builder);
    builder->append(kConfigServerConnectionString, configServerConnectionString.toString());
    builder->append(kFromShardId, fromShardId);
    builder->append(kToShardId, toShardId);
    builder->append(kChunkMinKey, chunkMinKey);
    builder->append(kChunkMaxKey, chunkMaxKey);
    builder->append(kMaxChunkSizeBytes, static_cast<long long>(maxChunkSizeBytes));
    secondaryThrottle.append(builder);
    builder->append(kWaitForDelete, waitForDelete);
}

}  // namespace mongo
