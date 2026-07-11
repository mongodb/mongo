// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/start_chunk_clone_request.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/idl/idl_parser.h"

#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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
// The donor chunk that encloses the migrated range (equal to it for a whole-chunk move, wider for a
// moveRange that splits the chunk), as a {min, max} sub-object. The recipient uses this span for
// the shard-catalog PIT-reachability check, and its presence is the signal that the migration is on
// the authoritative path. Absent on requests from a pre-upgrade donor and on the legacy path.
const char kEnclosingChunk[] = "enclosingChunk";
const char kShardKeyPattern[] = "shardKeyPattern";
// TODO (SERVER-127253): Remove this once v9.0 branches out.
const char kIsAuthoritative[] = "isAuthoritative";

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
        Status status = bsonExtractTypedField(obj, kChunkMinKey, BSONType::object, &elem);
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
        Status status = bsonExtractTypedField(obj, kChunkMaxKey, BSONType::object, &elem);
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
        Status status = bsonExtractTypedField(obj, kShardKeyPattern, BSONType::object, &elem);
        if (!status.isOK()) {
            return status;
        }

        request._shardKeyPattern = elem.Obj().getOwned();

        if (request._shardKeyPattern.isEmpty()) {
            return Status(ErrorCodes::UnsupportedFormat, "The shard key pattern cannot be empty");
        }
    }

    {
        // Optional: absent on requests from a pre-upgrade donor and on the legacy path. When
        // present it carries the enclosing donor chunk used for the recipient's PIT-reachability
        // check and marks the migration as being on the authoritative path.
        BSONElement elem;
        Status status = bsonExtractTypedField(obj, kEnclosingChunk, BSONType::object, &elem);
        if (status.isOK()) {
            const auto range = ChunkRange::fromBSON(elem.Obj());
            request._enclosingChunk =
                ChunkRange(range.getMin().getOwned(), range.getMax().getOwned());
        } else if (status != ErrorCodes::NoSuchKey) {
            return status;
        }
    }

    {
        // Absent on the legacy path, so default to false to preserve the pre-existing refresh
        // behavior when the donor does not send the field.
        Status status = bsonExtractBooleanFieldWithDefault(
            obj, kIsAuthoritative, false, &request._isAuthoritative);
        if (!status.isOK()) {
            return status;
        }
    }

    request._migrationId = UUID::parse(obj);
    request._lsid =
        LogicalSessionId::parse(obj[kLsid].Obj(), IDLParserContext("StartChunkCloneRequest"));
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
    const MigrationSecondaryThrottleOptions& secondaryThrottle,
    const boost::optional<ChunkRange>& enclosingChunk,
    bool isAuthoritative) {
    invariant(builder->asTempObj().isEmpty());
    invariant(nss.isValid());
    invariant(fromShardConnectionString.isValid());

    builder->append(kRecvChunkStart,
                    NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));

    migrationId.appendToBuilder(builder, kMigrationId);
    builder->append(kLsid, lsid.toBSON());
    builder->append(kTxnNumber, txnNumber);

    sessionId.append(builder);
    builder->append(kFromShardConnectionString, fromShardConnectionString.toString());
    builder->append(kFromShardId, fromShardId.toString());
    builder->append(kToShardId, toShardId.toString());
    builder->append(kChunkMinKey, chunkMinKey);
    builder->append(kChunkMaxKey, chunkMaxKey);
    if (enclosingChunk) {
        builder->append(kEnclosingChunk, enclosingChunk->toBSON());
    }
    builder->append(kShardKeyPattern, shardKeyPattern);
    // TODO (SERVER-127253): Remove this once v9.0 branches out.
    builder->append(kIsAuthoritative, isAuthoritative);
    secondaryThrottle.append(builder);
}

}  // namespace mongo
