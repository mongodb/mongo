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

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/s/request_types/migration_secondary_throttle_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class BSONObjBuilder;
template <typename T>
class StatusWith;

/**
 * Parses the arguments for a start chunk clone operation.
 */
class StartChunkCloneRequest {
public:
    static constexpr auto kSupportsCriticalSectionDuringCatchUp =
        "supportsCriticalSectionDuringCatchUp"_sd;

    /**
     * Parses the input command and produces a request corresponding to its arguments.
     */
    static StatusWith<StartChunkCloneRequest> createFromCommand(NamespaceString nss,
                                                                const BSONObj& obj);

    /**
     * Constructs a start chunk clone command with the specified parameters and writes it to the
     * builder, without closing the builder. The builder must be empty, but callers are free to
     * append more fields once the command has been constructed.
     */
    static void appendAsCommand(BSONObjBuilder* builder,
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
                                const MigrationSecondaryThrottleOptions& secondaryThrottle);

    const NamespaceString& getNss() const {
        return _nss;
    }

    const MigrationSessionId& getSessionId() const {
        return _sessionId;
    }

    const ConnectionString& getFromShardConnectionString() const {
        return _fromShardCS;
    }

    bool hasMigrationId() const {
        return _migrationId.is_initialized();
    }

    const UUID& getMigrationId() const {
        invariant(_migrationId);
        return *_migrationId;
    }

    const LogicalSessionId& getLsid() const {
        return _lsid;
    }

    TxnNumber getTxnNumber() const {
        return _txnNumber;
    }

    const ShardId& getFromShardId() const {
        return _fromShardId;
    }

    const ShardId& getToShardId() const {
        return _toShardId;
    }

    const BSONObj& getMinKey() const {
        return _minKey;
    }

    const BSONObj& getMaxKey() const {
        return _maxKey;
    }

    const BSONObj& getShardKeyPattern() const {
        return _shardKeyPattern;
    }

    const MigrationSecondaryThrottleOptions& getSecondaryThrottle() const {
        return _secondaryThrottle;
    }

private:
    StartChunkCloneRequest(NamespaceString nss,
                           MigrationSessionId sessionId,
                           MigrationSecondaryThrottleOptions secondaryThrottle);

    // The collection for which this request applies
    NamespaceString _nss;

    boost::optional<UUID> _migrationId;
    LogicalSessionId _lsid;
    TxnNumber _txnNumber{kUninitializedTxnNumber};

    // The session id of this migration
    MigrationSessionId _sessionId;

    // The source host and port
    ConnectionString _fromShardCS;

    // The recipient and destination shard IDs.
    ShardId _fromShardId;
    ShardId _toShardId;

    // Exact min and max key of the chunk being moved
    BSONObj _minKey;
    BSONObj _maxKey;

    // Shard key pattern used by the collection
    BSONObj _shardKeyPattern;

    // The parsed secondary throttle options
    MigrationSecondaryThrottleOptions _secondaryThrottle;
};

}  // namespace mongo
