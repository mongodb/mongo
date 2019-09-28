
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kWrite

#include "mongo/platform/basic.h"

#include "mongo/db/session_catalog.h"

#include <boost/optional.hpp>

#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/kill_sessions_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const auto sessionTransactionTableDecoration = ServiceContext::declareDecoration<SessionCatalog>();

const auto operationSessionDecoration =
    OperationContext::declareDecoration<boost::optional<ScopedCheckedOutSession>>();

const auto kIdProjection = BSON(SessionTxnRecord::kSessionIdFieldName << 1);
const auto kSortById = BSON(SessionTxnRecord::kSessionIdFieldName << 1);
const auto kLastWriteDateFieldName = SessionTxnRecord::kLastWriteDateFieldName;

/**
 * Removes the specified set of session ids from the persistent sessions collection and returns the
 * number of sessions actually removed.
 */
int removeSessionsTransactionRecords(OperationContext* opCtx,
                                     SessionsCollection& sessionsCollection,
                                     const LogicalSessionIdSet& sessionIdsToRemove) {
    if (sessionIdsToRemove.empty())
        return 0;

    // From the passed-in sessions, find the ones which are actually expired/removed
    auto expiredSessionIds =
        uassertStatusOK(sessionsCollection.findRemovedSessions(opCtx, sessionIdsToRemove));

    if (expiredSessionIds.empty())
        return 0;

    // Remove the session ids from the on-disk catalog
    write_ops::Delete deleteOp(NamespaceString::kSessionTransactionsTableNamespace);
    deleteOp.setWriteCommandBase([] {
        write_ops::WriteCommandBase base;
        base.setOrdered(false);
        return base;
    }());
    deleteOp.setDeletes([&] {
        std::vector<write_ops::DeleteOpEntry> entries;
        for (const auto& lsid : expiredSessionIds) {
            entries.emplace_back([&] {
                write_ops::DeleteOpEntry entry;
                entry.setQ(BSON(LogicalSessionRecord::kIdFieldName << lsid.toBSON()));
                entry.setMulti(false);
                return entry;
            }());
        }
        return entries;
    }());

    BSONObj result;

    DBDirectClient client(opCtx);
    client.runCommand(NamespaceString::kSessionTransactionsTableNamespace.db().toString(),
                      deleteOp.toBSON({}),
                      result);

    BatchedCommandResponse response;
    std::string errmsg;
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "Failed to parse response " << result,
            response.parseBSON(result, &errmsg));
    uassertStatusOK(response.getTopLevelStatus());

    return response.getN();
}

}  // namespace

SessionCatalog::~SessionCatalog() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    for (const auto& entry : _txnTable) {
        auto& sri = entry.second;
        invariant(!sri->checkedOut);
    }
}

void SessionCatalog::reset_forTest() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _txnTable.clear();
}

SessionCatalog* SessionCatalog::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

SessionCatalog* SessionCatalog::get(ServiceContext* service) {
    auto& sessionTransactionTable = sessionTransactionTableDecoration(service);
    return &sessionTransactionTable;
}

boost::optional<UUID> SessionCatalog::getTransactionTableUUID(OperationContext* opCtx) {
    AutoGetCollection autoColl(opCtx, NamespaceString::kSessionTransactionsTableNamespace, MODE_IS);

    const auto coll = autoColl.getCollection();
    if (coll == nullptr) {
        return boost::none;
    }

    return coll->uuid();
}

int SessionCatalog::reapSessionsOlderThan(OperationContext* opCtx,
                                          SessionsCollection& sessionsCollection,
                                          Date_t possiblyExpired) {
    const auto catalog = SessionCatalog::get(opCtx);
    catalog->_reapInMemorySessionsOlderThan(opCtx, sessionsCollection, possiblyExpired);

    // The "unsafe" check for primary below is a best-effort attempt to ensure that the on-disk
    // state reaping code doesn't run if the node is secondary and cause log spam. It is a work
    // around the fact that the logical sessions cache is not registered to listen for replication
    // state changes.
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->canAcceptWritesForDatabase_UNSAFE(opCtx, NamespaceString::kConfigDb))
        return 0;

    // Scan for records older than the minimum lifetime and uses a sort to walk the '_id' index
    DBDirectClient client(opCtx);
    auto cursor =
        client.query(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                     Query(BSON(kLastWriteDateFieldName << LT << possiblyExpired)).sort(kSortById),
                     0,
                     0,
                     &kIdProjection);

    // The max batch size is chosen so that a single batch won't exceed the 16MB BSON object size
    // limit
    const int kMaxBatchSize = 10'000;

    LogicalSessionIdSet lsids;
    int numReaped = 0;
    while (cursor->more()) {
        auto transactionSession = SessionsCollectionFetchResultIndividualResult::parse(
            "TransactionSession"_sd, cursor->next());

        lsids.insert(transactionSession.get_id());
        if (lsids.size() > kMaxBatchSize) {
            numReaped += removeSessionsTransactionRecords(opCtx, sessionsCollection, lsids);
            lsids.clear();
        }
    }

    numReaped += removeSessionsTransactionRecords(opCtx, sessionsCollection, lsids);

    return numReaped;
}

void SessionCatalog::onStepUp(OperationContext* opCtx) {
    invalidateSessions(opCtx, boost::none);

    DBDirectClient client(opCtx);

    const size_t initialExtentSize = 0;
    const bool capped = false;
    const bool maxSize = 0;

    BSONObj result;

    if (client.createCollection(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                                initialExtentSize,
                                capped,
                                maxSize,
                                &result)) {
        return;
    }

    const auto status = getStatusFromCommandResult(result);

    if (status == ErrorCodes::NamespaceExists) {
        return;
    }

    uassertStatusOKWithContext(status,
                               str::stream()
                                   << "Failed to create the "
                                   << NamespaceString::kSessionTransactionsTableNamespace.ns()
                                   << " collection");
}

ScopedCheckedOutSession SessionCatalog::checkOutSession(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(opCtx->getLogicalSessionId());

    const auto lsid = *opCtx->getLogicalSessionId();

    stdx::unique_lock<stdx::mutex> ul(_mutex);

    auto sri = _getOrCreateSessionRuntimeInfo(ul, opCtx, lsid);

    // Wait until the session is no longer checked out
    opCtx->waitForConditionOrInterrupt(
        sri->availableCondVar, ul, [&sri]() { return !sri->checkedOut; });

    invariant(!sri->checkedOut);
    sri->checkedOut = true;
    sri->lastCheckout = Date_t::now();

    return ScopedCheckedOutSession(opCtx, ScopedSession(std::move(sri)));
}

ScopedSession SessionCatalog::getOrCreateSession(OperationContext* opCtx,
                                                 const LogicalSessionId& lsid) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getLogicalSessionId());
    invariant(!opCtx->getTxnNumber());

    auto ss = [&] {
        stdx::unique_lock<stdx::mutex> ul(_mutex);
        return ScopedSession(_getOrCreateSessionRuntimeInfo(ul, opCtx, lsid));
    }();

    // Perform the refresh outside of the mutex
    ss->refreshFromStorageIfNeeded(opCtx);

    return ss;
}

void SessionCatalog::invalidateSessions(OperationContext* opCtx,
                                        boost::optional<BSONObj> singleSessionDoc) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    bool isReplSet = replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
    if (isReplSet) {
        uassert(40528,
                str::stream() << "Direct writes against "
                              << NamespaceString::kSessionTransactionsTableNamespace.ns()
                              << " cannot be performed using a transaction or on a session.",
                !opCtx->getLogicalSessionId());
    }

    stdx::lock_guard<stdx::mutex> lg(_mutex);

    if (singleSessionDoc) {
        const auto lsid = LogicalSessionId::parse(IDLParserErrorContext("lsid"),
                                                  singleSessionDoc->getField("_id").Obj());

        auto it = _txnTable.find(lsid);
        if (it != _txnTable.end()) {
            _invalidateSession(lg, it);
        }
    } else {
        auto it = _txnTable.begin();
        while (it != _txnTable.end()) {
            _invalidateSession(lg, it++);
        }
    }
}

void SessionCatalog::scanSessions(OperationContext* opCtx,
                                  const SessionKiller::Matcher& matcher,
                                  stdx::function<void(OperationContext*, Session*)> workerFn) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    LOG(2) << "Beginning scanSessions. Scanning " << _txnTable.size() << " sessions.";

    for (auto it = _txnTable.begin(); it != _txnTable.end(); ++it) {
        // TODO SERVER-33850: Rename KillAllSessionsByPattern and
        // ScopedKillAllSessionsByPatternImpersonator to not refer to session kill.
        if (const KillAllSessionsByPattern* pattern = matcher.match(it->first)) {
            ScopedKillAllSessionsByPatternImpersonator impersonator(opCtx, *pattern);
            workerFn(opCtx, &(it->second->txnState));
        }
    }
}

size_t SessionCatalog::size() const {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    return _txnTable.size();
}

std::shared_ptr<SessionCatalog::SessionRuntimeInfo> SessionCatalog::_getOrCreateSessionRuntimeInfo(
    WithLock, OperationContext* opCtx, const LogicalSessionId& lsid) {
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    auto it = _txnTable.find(lsid);
    if (it == _txnTable.end()) {
        it = _txnTable.emplace(lsid, std::make_shared<SessionRuntimeInfo>(lsid)).first;
    }

    return it->second;
}

void SessionCatalog::_releaseSession(const LogicalSessionId& lsid) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    auto it = _txnTable.find(lsid);
    invariant(it != _txnTable.end());

    auto& sri = it->second;
    invariant(sri->checkedOut);

    sri->checkedOut = false;
    sri->availableCondVar.notify_one();
}

void SessionCatalog::_invalidateSession(WithLock, SessionRuntimeInfoMap::iterator it) {
    auto& sri = it->second;
    sri->txnState.invalidate();

    // We cannot remove checked-out sessions from the cache, because operations expect to find them
    // there to check back in
    if (!sri->checkedOut) {
        _txnTable.erase(it);
    }
}

void SessionCatalog::_reapInMemorySessionsOlderThan(OperationContext* opCtx,
                                                    SessionsCollection& sessionsCollection,
                                                    Date_t possiblyExpired) {
    LogicalSessionIdSet possiblyExpiredLsids;
    {
        stdx::unique_lock<stdx::mutex> ul(_mutex);
        for (auto& entry : _txnTable) {
            auto& session = entry.second;
            if (session->lastCheckout < possiblyExpired) {
                possiblyExpiredLsids.insert(entry.first);
            }
        }
    }

    // From the passed-in sessions, find the ones which are actually expired/removed
    auto expiredSessionIds =
        uassertStatusOK(sessionsCollection.findRemovedSessions(opCtx, possiblyExpiredLsids));

    stdx::lock_guard<stdx::mutex> lg(_mutex);

    // Remove the session ids from the in-memory catalog
    for (const auto& lsid : expiredSessionIds) {
        auto it = _txnTable.find(lsid);
        if (it == _txnTable.end())
            continue;

        _invalidateSession(lg, it);
    }
}

OperationContextSession::OperationContextSession(OperationContext* opCtx, bool checkOutSession)
    : _opCtx(opCtx) {

    if (!opCtx->getLogicalSessionId()) {
        return;
    }

    if (!checkOutSession) {
        return;
    }

    auto& checkedOutSession = operationSessionDecoration(opCtx);
    if (!checkedOutSession) {
        auto sessionTransactionTable = SessionCatalog::get(opCtx);
        auto scopedCheckedOutSession = sessionTransactionTable->checkOutSession(opCtx);
        // We acquire a Client lock here to guard the construction of this session so that
        // references to this session are safe to use while the lock is held.
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        checkedOutSession.emplace(std::move(scopedCheckedOutSession));
    } else {
        // The only reason to be trying to check out a session when you already have a session
        // checked out is if you're in DBDirectClient.
        invariant(opCtx->getClient()->isInDirectClient());
        return;
    }

    const auto session = checkedOutSession->get();
    invariant(opCtx->getLogicalSessionId() == session->getSessionId());

    checkedOutSession->get()->refreshFromStorageIfNeeded(opCtx);

    session->setCurrentOperation(opCtx);
}

OperationContextSession::~OperationContextSession() {
    // Only release the checked out session at the end of the top-level request from the client,
    // not at the end of a nested DBDirectClient call.
    if (_opCtx->getClient()->isInDirectClient()) {
        return;
    }

    auto& checkedOutSession = operationSessionDecoration(_opCtx);
    if (checkedOutSession) {
        checkedOutSession->get()->clearCurrentOperation();
    }
    // We acquire a Client lock here to guard the destruction of this session so that references to
    // this session are safe to use while the lock is held.
    stdx::lock_guard<Client> lk(*_opCtx->getClient());
    checkedOutSession.reset();
}

Session* OperationContextSession::get(OperationContext* opCtx) {
    auto& checkedOutSession = operationSessionDecoration(opCtx);
    if (checkedOutSession) {
        return checkedOutSession->get();
    }

    return nullptr;
}

}  // namespace mongo
