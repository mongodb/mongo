/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/transaction_reaper.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/sessions_collection.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

constexpr Minutes kTransactionRecordMinimumLifetime(30);

/**
 * The minimum lifetime for a transaction record is how long it has to have lived on the server
 * before we'll consider it for cleanup.  This is effectively the window for how long it is
 * permissible for a mongos to hang before we're willing to accept a failure of the retryable write
 * subsystem.
 *
 * Specifically, we imagine that a client connects to one mongos on a session and performs a
 * retryable write.  That mongos hangs.  Then the client connects to a new mongos on the same
 * session and successfully executes its write.  After a day passes, the session will time out,
 * cleaning up the retryable write.  Then the original mongos wakes up, vivifies the session and
 * executes the write (because all records of the session + transaction have been deleted).
 *
 * So the write is performed twice, which is unavoidable without losing session vivification and/or
 * requiring synchronized clocks all the way out to the client.  In lieu of that we provide a weaker
 * guarantee after the minimum transaction lifetime.
 */
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(TransactionRecordMinimumLifetimeMinutes,
                                      int,
                                      kTransactionRecordMinimumLifetime.count());

const auto kIdProjection = BSON(SessionTxnRecord::kSessionIdFieldName << 1);
const auto kSortById = BSON(SessionTxnRecord::kSessionIdFieldName << 1);
const auto kLastWriteDateFieldName = SessionTxnRecord::kLastWriteDateFieldName;

/**
 * Makes the query we'll use to scan the transactions table.
 *
 * Scans for records older than the minimum lifetime and uses a sort to walk the index and attempt
 * to pull records likely to be on the same chunks (because they sort near each other).
 */
Query makeQuery(Date_t now) {
    const Date_t possiblyExpired(now - Minutes(TransactionRecordMinimumLifetimeMinutes));
    Query query(BSON(kLastWriteDateFieldName << LT << possiblyExpired));
    query.sort(kSortById);
    return query;
}

/**
 * Our impl is templatized on a type which handles the lsids we see.  It provides the top level
 * scaffolding for figuring out if we're the primary node responsible for the transaction table and
 * invoking the handler.
 *
 * The handler here will see all of the possibly expired txn ids in the transaction table and will
 * have a lifetime associated with a single call to reap.
 */
template <typename Handler>
class TransactionReaperImpl final : public TransactionReaper {
public:
    TransactionReaperImpl(std::shared_ptr<SessionsCollection> collection)
        : _collection(std::move(collection)) {}

    int reap(OperationContext* opCtx) override {
        auto const coord = mongo::repl::ReplicationCoordinator::get(opCtx);

        Handler handler(opCtx, *_collection);
        if (!handler.initialize()) {
            return 0;
        }

        AutoGetCollection autoColl(
            opCtx, NamespaceString::kSessionTransactionsTableNamespace, MODE_IS);

        // Only start reaping if the shard or config server node is currently the primary
        if (!coord->canAcceptWritesForDatabase(
                opCtx, NamespaceString::kSessionTransactionsTableNamespace.db())) {
            return 0;
        }

        DBDirectClient client(opCtx);

        auto query = makeQuery(opCtx->getServiceContext()->getFastClockSource()->now());
        auto cursor = client.query(
            NamespaceString::kSessionTransactionsTableNamespace.ns(), query, 0, 0, &kIdProjection);

        while (cursor->more()) {
            auto transactionSession = SessionsCollectionFetchResultIndividualResult::parse(
                "TransactionSession"_sd, cursor->next());

            handler.handleLsid(transactionSession.get_id());
        }

        // Before the handler goes out of scope, flush its last batch to disk and collect stats.
        return handler.finalize();
    }

private:
    std::shared_ptr<SessionsCollection> _collection;
};

/**
 * Removes the specified set of session ids from the persistent sessions collection and returns the
 * number of sessions actually removed.
 */
int removeSessionsRecords(OperationContext* opCtx,
                          SessionsCollection& sessionsCollection,
                          const LogicalSessionIdSet& sessionIdsToRemove) {
    if (sessionIdsToRemove.empty()) {
        return 0;
    }

    Locker* locker = opCtx->lockState();

    Locker::LockSnapshot snapshot;
    invariant(locker->saveLockStateAndUnlock(&snapshot));

    const auto guard = MakeGuard([&] {
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        locker->restoreLockState(opCtx, snapshot);
    });

    // Top-level locks are freed, release any potential low-level (storage engine-specific
    // locks). If we are yielding, we are at a safe place to do so.
    opCtx->recoveryUnit()->abandonSnapshot();

    // Track the number of yields in CurOp.
    CurOp::get(opCtx)->yielded();

    auto removed =
        uassertStatusOK(sessionsCollection.findRemovedSessions(opCtx, sessionIdsToRemove));
    uassertStatusOK(sessionsCollection.removeTransactionRecords(opCtx, removed));

    return removed.size();
}

/**
 * The repl impl is simple, just pass along to the sessions collection for checking ids locally
 */
class ReplHandler {
public:
    ReplHandler(OperationContext* opCtx, SessionsCollection& sessionsCollection)
        : _opCtx(opCtx), _sessionsCollection(sessionsCollection) {}

    bool initialize() {
        return true;
    }

    void handleLsid(const LogicalSessionId& lsid) {
        _batch.insert(lsid);

        if (_batch.size() > write_ops::kMaxWriteBatchSize) {
            _numReaped += removeSessionsRecords(_opCtx, _sessionsCollection, _batch);
            _batch.clear();
        }
    }

    int finalize() {
        invariant(!_finalized);
        _finalized = true;

        _numReaped += removeSessionsRecords(_opCtx, _sessionsCollection, _batch);
        return _numReaped;
    }

private:
    OperationContext* const _opCtx;
    SessionsCollection& _sessionsCollection;

    LogicalSessionIdSet _batch;

    int _numReaped{0};

    bool _finalized{false};
};

/**
 * The sharding impl is a little fancier.  Try to bucket by shard id, to avoid doing repeated small
 * scans.
 */
class ShardedHandler {
public:
    ShardedHandler(OperationContext* opCtx, SessionsCollection& sessionsCollection)
        : _opCtx(opCtx), _sessionsCollection(sessionsCollection) {}

    // Returns false if the sessions collection is not set up.
    bool initialize() {
        auto routingInfo =
            uassertStatusOK(Grid::get(_opCtx)->catalogCache()->getCollectionRoutingInfo(
                _opCtx, NamespaceString::kLogicalSessionsNamespace));
        _cm = routingInfo.cm();
        return !!_cm;
    }

    void handleLsid(const LogicalSessionId& lsid) {
        invariant(_cm);
        const auto chunk = _cm->findIntersectingChunkWithSimpleCollation(lsid.toBSON());
        const auto shardId = chunk.getShardId();

        auto& lsids = _shards[shardId];
        lsids.insert(lsid);

        if (lsids.size() > write_ops::kMaxWriteBatchSize) {
            _numReaped += removeSessionsRecords(_opCtx, _sessionsCollection, lsids);
            _shards.erase(shardId);
        }
    }

    int finalize() {
        invariant(!_finalized);
        _finalized = true;

        for (const auto& pair : _shards) {
            _numReaped += removeSessionsRecords(_opCtx, _sessionsCollection, pair.second);
        }

        return _numReaped;
    }

private:
    OperationContext* const _opCtx;
    SessionsCollection& _sessionsCollection;

    std::shared_ptr<ChunkManager> _cm;

    stdx::unordered_map<ShardId, LogicalSessionIdSet, ShardId::Hasher> _shards;
    int _numReaped{0};

    bool _finalized{false};
};

}  // namespace

std::unique_ptr<TransactionReaper> TransactionReaper::make(
    Type type, std::shared_ptr<SessionsCollection> collection) {
    switch (type) {
        case Type::kReplicaSet:
            return stdx::make_unique<TransactionReaperImpl<ReplHandler>>(std::move(collection));
        case Type::kSharded:
            return stdx::make_unique<TransactionReaperImpl<ShardedHandler>>(std::move(collection));
    }
    MONGO_UNREACHABLE;
}

TransactionReaper::~TransactionReaper() = default;

}  // namespace mongo
