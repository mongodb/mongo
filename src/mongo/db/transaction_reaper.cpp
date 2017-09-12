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
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/session_txn_record.h"
#include "mongo/db/sessions_collection.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/destructor_guard.h"

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

/**
 * Makes the query we'll use to scan the transactions table.
 *
 * Scans for records older than the minimum lifetime and uses a sort to walk the index and attempt
 * to pull records likely to be on the same chunks (because they sort near each other).
 */
Query makeQuery(Date_t now) {
    Timestamp possiblyExpired(
        duration_cast<Seconds>(
            (now - Minutes(TransactionRecordMinimumLifetimeMinutes)).toDurationSinceEpoch()),
        0);
    BSONObjBuilder bob;
    {
        BSONObjBuilder subbob(bob.subobjStart(SessionTxnRecord::kLastWriteOpTimeTsFieldName));
        subbob.append("$lt", possiblyExpired);
    }
    Query query(bob.obj());
    query.sort(kSortById);
    return query;
}

/**
 * Our impl is templatized on a type which handles the lsids we see.  It provides the top level
 * scaffolding for figuring out if we're the primary node responsible for the transaction table and
 * invoking the hanlder.
 *
 * The handler here will see all of the possibly expired txn ids in the transaction table and will
 * have a lifetime associated with a single call to reap.
 */
template <typename Handler>
class TransactionReaperImpl final : public TransactionReaper {
public:
    TransactionReaperImpl(std::shared_ptr<SessionsCollection> collection)
        : _collection(std::move(collection)) {}

    void reap(OperationContext* opCtx) override {
        Handler handler(opCtx, _collection.get());

        Lock::DBLock lk(opCtx, SessionsCollection::kSessionsDb, MODE_IS);
        Lock::CollectionLock lock(opCtx->lockState(), SessionsCollection::kSessionsFullNS, MODE_IS);

        auto coord = mongo::repl::ReplicationCoordinator::get(opCtx);
        if (coord->canAcceptWritesForDatabase(
                opCtx, NamespaceString::kSessionTransactionsTableNamespace.db())) {
            DBDirectClient client(opCtx);

            auto query = makeQuery(opCtx->getServiceContext()->getFastClockSource()->now());
            auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                                       query,
                                       0,
                                       0,
                                       &kIdProjection);

            while (cursor->more()) {
                auto transactionSession = SessionsCollectionFetchResultIndividualResult::parse(
                    "TransactionSession"_sd, cursor->next());

                handler.handleLsid(transactionSession.get_id());
            }
        }
    }

private:
    std::shared_ptr<SessionsCollection> _collection;
};

void handleBatchHelper(SessionsCollection* sessionsCollection,
                       OperationContext* opCtx,
                       const LogicalSessionIdSet& batch) {
    auto removed = uassertStatusOK(sessionsCollection->findRemovedSessions(opCtx, batch));
    uassertStatusOK(sessionsCollection->removeTransactionRecords(opCtx, removed));
}

/**
 * The repl impl is simple, just pass along to the sessions collection for checking ids locally
 */
class ReplHandler {
public:
    ReplHandler(OperationContext* opCtx, SessionsCollection* collection)
        : _opCtx(opCtx), _sessionsCollection(collection) {}

    ~ReplHandler() {
        DESTRUCTOR_GUARD([&] { handleBatchHelper(_sessionsCollection, _opCtx, _batch); }());
    }

    void handleLsid(const LogicalSessionId& lsid) {
        _batch.insert(lsid);
        if (_batch.size() > write_ops::kMaxWriteBatchSize) {
            handleBatchHelper(_sessionsCollection, _opCtx, _batch);
            _batch.clear();
        }
    }

private:
    OperationContext* _opCtx;
    SessionsCollection* _sessionsCollection;

    LogicalSessionIdSet _batch;
};

/**
 * The sharding impl is a little fancier.  Try to bucket by shard id, to avoid doing repeated small
 * scans.
 */
class ShardedHandler {
public:
    ShardedHandler(OperationContext* opCtx, SessionsCollection* collection)
        : _opCtx(opCtx), _sessionsCollection(collection) {}

    ~ShardedHandler() {
        DESTRUCTOR_GUARD([&] {
            for (const auto& pair : _shards) {
                handleBatchHelper(_sessionsCollection, _opCtx, pair.second);
            }
        }());
    }

    void handleLsid(const LogicalSessionId& lsid) {
        // There are some lifetime issues with when the reaper starts up versus when the grid is
        // available.  Moving routing info fetching until after we have a transaction moves us past
        // the problem.
        //
        // Also, we should only need the chunk case, but that'll wait until the sessions table is
        // actually sharded.
        if (!(_cm || _primary)) {
            auto routingInfo =
                uassertStatusOK(Grid::get(_opCtx)->catalogCache()->getCollectionRoutingInfo(
                    _opCtx, SessionsCollection::kSessionsFullNS));
            _cm = routingInfo.cm();
            _primary = routingInfo.primary();
        }
        ShardId shardId;
        if (_cm) {
            auto chunk = _cm->findIntersectingChunkWithSimpleCollation(lsid.toBSON());
            shardId = chunk->getShardId();
        } else {
            shardId = _primary->getId();
        }
        auto& lsids = _shards[shardId];
        lsids.insert(lsid);
        if (lsids.size() > write_ops::kMaxWriteBatchSize) {
            handleBatchHelper(_sessionsCollection, _opCtx, lsids);
            _shards.erase(shardId);
        }
    }

private:
    OperationContext* _opCtx;
    SessionsCollection* _sessionsCollection;
    std::shared_ptr<ChunkManager> _cm;
    std::shared_ptr<Shard> _primary;

    stdx::unordered_map<ShardId, LogicalSessionIdSet, ShardId::Hasher> _shards;
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
