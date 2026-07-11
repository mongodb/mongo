// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/global_catalog/type_chunk_range.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/s/request_types/migration_secondary_throttle_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <string>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

class BSONObjBuilder;
template <typename T>
class StatusWith;

/**
 * Parses the arguments for a start chunk clone operation.
 */
class StartChunkCloneRequest {
public:
    static constexpr auto kSupportsCriticalSectionDuringCatchUp =
        "supportsCriticalSectionDuringCatchUp"sv;

    /**
     * Parses the input command and produces a request corresponding to its arguments.
     */
    static StatusWith<StartChunkCloneRequest> createFromCommand(NamespaceString nss,
                                                                const BSONObj& obj);

    /**
     * Constructs a start chunk clone command with the specified parameters and writes it to the
     * builder, without closing the builder. The builder must be empty, but callers are free to
     * append more fields once the command has been constructed.
     *
     * TODO (SERVER-127253) Make enclosingChunk parameter non-optional once v9.0 branches out.
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
                                const MigrationSecondaryThrottleOptions& secondaryThrottle,
                                const boost::optional<ChunkRange>& enclosingChunk,
                                bool isAuthoritative);

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

    // The donor chunk enclosing the migrated range.
    // Present only on the authoritative path (driven by the MoveRangeCoordinator), so its presence
    // is also the signal that the recipient should run the shard-catalog PIT-reachability check.
    const boost::optional<ChunkRange>& getEnclosingChunk() const {
        return _enclosingChunk;
    }

    const BSONObj& getShardKeyPattern() const {
        return _shardKeyPattern;
    }

    const MigrationSecondaryThrottleOptions& getSecondaryThrottle() const {
        return _secondaryThrottle;
    }

    // Whether the migration is driven by a MoveRangeCoordinator, which commits authoritatively.
    // On the authoritative path the recipient does not need to force a filtering-metadata refresh
    // when it starts receiving the chunk, because the post-migration metadata is installed into the
    // shard catalog directly. Absent on the legacy path, which defaults this to false.
    // TODO (SERVER-127253): Remove this once v9.0 branches out.
    bool isAuthoritative() const {
        return _isAuthoritative;
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

    // The donor chunk that encloses the migrated range (equal to it for a whole-chunk move, wider
    // for a moveRange that splits the chunk). Absent on requests from a pre-upgrade donor and on
    // the legacy (non-authoritative) path.
    // TODO (SERVER-127253) Make this parameter non-optional once v9.0 branches out.
    boost::optional<ChunkRange> _enclosingChunk;

    // Shard key pattern used by the collection
    BSONObj _shardKeyPattern;

    // The parsed secondary throttle options
    MigrationSecondaryThrottleOptions _secondaryThrottle;

    // Whether the migration commits authoritatively (driven by a MoveRangeCoordinator).
    // TODO (SERVER-127253): Remove this once v9.0 branches out.
    bool _isAuthoritative{false};
};

}  // namespace mongo
