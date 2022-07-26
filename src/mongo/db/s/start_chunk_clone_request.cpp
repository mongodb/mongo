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

#include "mongo/db/s/start_chunk_clone_request.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/commands/feature_compatibility_version.h"

namespace mongo {
namespace {

const char kRecvChunkStart[] = "_recvChunkStart";
const char kFromShardConnectionString[] = "from";
// Note: The UUID parsing code relies on this field being named 'uuid'.
const char kMigrationId[] = "uuid";
const char kLsid[] = "lsid";
const char kTxnNumber[] = "txnNumber";
const char kFromShardId[] = "fromShardName";
const char kToShardId[] = "toShardName";
const char kChunkMinKey[] = "min";
const char kChunkMaxKey[] = "max";
const char kShardKeyPattern[] = "shardKeyPattern";

}  // namespace

StartChunkCloneRequest::StartChunkCloneRequest(NamespaceString nss,
                                               MigrationSessionId sessionId,
                                               MigrationSecondaryThrottleOptions secondaryThrottle)
    : _nss(std::move(nss)),
      _sessionId(std::move(sessionId)),
      _secondaryThrottle(std::move(secondaryThrottle)) {}

StatusWith<StartChunkCloneRequest> StartChunkCloneRequest::createFromCommand(NamespaceString nss,
                                                                             const BSONObj& obj) {
    auto secondaryThrottleStatus = MigrationSecondaryThrottleOptions::createFromCommand(obj);
    if (!secondaryThrottleStatus.isOK()) {
        return secondaryThrottleStatus.getStatus();
    }

    auto sessionIdStatus = MigrationSessionId::extractFromBSON(obj);
    if (!sessionIdStatus.isOK()) {
        return sessionIdStatus.getStatus();
    }

    StartChunkCloneRequest request(std::move(nss),
                                   std::move(sessionIdStatus.getValue()),
                                   std::move(secondaryThrottleStatus.getValue()));

    {
        std::string fromShardConnectionString;
        Status status =
            bsonExtractStringField(obj, kFromShardConnectionString, &fromShardConnectionString);
        if (!status.isOK()) {
            return status;
        }

        auto fromShardConnectionStringStatus = ConnectionString::parse(fromShardConnectionString);
        if (!fromShardConnectionStringStatus.isOK()) {
            return fromShardConnectionStringStatus.getStatus();
        }

        request._fromShardCS = std::move(fromShardConnectionStringStatus.getValue());
    }

    {
        std::string fromShard;
        Status status = bsonExtractStringField(obj, kFromShardId, &fromShard);
        request._fromShardId = fromShard;
        if (!status.isOK()) {
            return status;
        }
    }

    {
        std::string toShard;
        Status status = bsonExtractStringField(obj, kToShardId, &toShard);
        request._toShardId = toShard;
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
        BSONElement elem;
        Status status = bsonExtractTypedField(obj, kShardKeyPattern, BSONType::Object, &elem);
        if (!status.isOK()) {
            return status;
        }

        request._shardKeyPattern = elem.Obj().getOwned();

        if (request._shardKeyPattern.isEmpty()) {
            return Status(ErrorCodes::UnsupportedFormat, "The shard key pattern cannot be empty");
        }
    }

    request._migrationId = UUID::parse(obj);
    request._lsid =
        LogicalSessionId::parse(IDLParserContext("StartChunkCloneRequest"), obj[kLsid].Obj());
    request._txnNumber = obj.getField(kTxnNumber).Long();

    return request;
}

void StartChunkCloneRequest::appendAsCommand(
    BSONObjBuilder* builder,
    const NamespaceString& nss,
    const UUID& migrationId,
    const LogicalSessionId& lsid,
    TxnNumber txnNumber,
    const MigrationSessionId& sessionId,
    const ConnectionString& fromShardConnectionString,
    const ShardId& fromShardId,
    const ShardId& toShardId,
    const BSONObj& chunkMinKey,
    const BSONObj& chunkMaxKey,
    const BSONObj& shardKeyPattern,
    const MigrationSecondaryThrottleOptions& secondaryThrottle) {
    invariant(builder->asTempObj().isEmpty());
    invariant(nss.isValid());
    invariant(fromShardConnectionString.isValid());

    builder->append(kRecvChunkStart, nss.ns());

    migrationId.appendToBuilder(builder, kMigrationId);
    builder->append(kLsid, lsid.toBSON());
    builder->append(kTxnNumber, txnNumber);

    sessionId.append(builder);
    builder->append(kFromShardConnectionString, fromShardConnectionString.toString());
    builder->append(kFromShardId, fromShardId.toString());
    builder->append(kToShardId, toShardId.toString());
    builder->append(kChunkMinKey, chunkMinKey);
    builder->append(kChunkMaxKey, chunkMaxKey);
    builder->append(kShardKeyPattern, shardKeyPattern);
    secondaryThrottle.append(builder);
}

}  // namespace mongo
