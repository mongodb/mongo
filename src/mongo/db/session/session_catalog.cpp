// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/session/session_catalog.h"

#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session_catalog_gen.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/observable_mutex_registry.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite


namespace mongo {
namespace {

const auto sessionTransactionTableDecoration = ServiceContext::declareDecoration<SessionCatalog>();

const auto operationSessionDecoration =
    OperationContext::declareDecoration<boost::optional<SessionCatalog::ScopedCheckedOutSession>>();

MONGO_FAIL_POINT_DEFINE(hangAfterIncrementingNumWaitingToCheckOut);

std::string provenanceToString(SessionCatalog::Provenance provenance) {
    switch (provenance) {
        case SessionCatalog::Provenance::kRouter:
            return "router";
        case SessionCatalog::Provenance::kParticipant:
            return "participant";
    }
    MONGO_UNREACHABLE;
}

// Bounds on the auto-sized count only; an explicit setting is used as given. Never zero: the
// partition lookup takes a hash modulo this count.
static constexpr size_t kMinAutoPartitions = 1;
static constexpr size_t kMaxAutoPartitions = 64;

// Auto-sizes from the cores this process may use when 'sessionCatalogPartitions' is 0.
size_t computeNumPartitions() {
    if (gSessionCatalogPartitions > 0) {
        return static_cast<size_t>(gSessionCatalogPartitions);
    }

    const auto numPartitions =
        std::clamp(static_cast<size_t>(2 * ProcessInfo::getNumAvailableCores()),
                   kMinAutoPartitions,
                   kMaxAutoPartitions);

    // So getParameter reports the partitions in use rather than 0.
    gSessionCatalogPartitions = static_cast<int>(numPartitions);

    return numPartitions;
}

}  // namespace

SessionCatalog::SessionCatalog() : _partitions(computeNumPartitions()) {
    for (size_t i = 0; i < _partitions.size(); ++i) {
        const auto label = fmt::format("partition_{}", i);
        ObservableMutexRegistry::get().add(
            "sessionCatalogMutex", _partitions[i].mutex(), std::string_view{label});
    }
}

SessionCatalog::~SessionCatalog() {
    for (auto& partition : _partitions) {
        Partition::Locked locked(partition);
        for (const auto& [_, sri] : locked.sessions()) {
            ObservableSession osession(locked, sri.get(), &sri->parentSession);
            invariant(!osession.hasCurrentOperation());
            invariant(!osession._killed());
        }
    }
}

void SessionCatalog::reset_forTest() {
    for (auto& partition : _partitions) {
        Partition::Locked locked(partition);
        _numParentSessions.fetchAndSubtract(locked.sessions().size());
        locked.sessions().clear();
    }
}

SessionCatalog* SessionCatalog::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

SessionCatalog* SessionCatalog::get(ServiceContext* service) {
    auto& sessionTransactionTable = sessionTransactionTableDecoration(service);
    return &sessionTransactionTable;
}

void SessionCatalog::KillToken::_returnIfArmed() {
    if (_catalog) {
        _catalog->_returnKill(*this);
    }
}

void SessionCatalog::_returnKill(WithLock lk, SessionRuntimeInfo* sri, KillToken& killToken) {
    invariant(killToken._catalog == this);

    sri->returnKill(lk);
    killToken._catalog = nullptr;
}

void SessionCatalog::_returnKill(KillToken& killToken) {
    const auto& lsid = killToken.lsidToKill();
    auto& partitionToLock = _partitionFor(lsid);

    // Returning a kill locks the session's partition, so a token destroyed by a thread which
    // already holds it would wait on itself forever. Report that as a leaked kill instead of
    // hanging. This misses a holder which is waiting on 'availableCondVar', since that releases the
    // mutex without dropping its 'Locked'.
    invariant(!partitionToLock.isLockedByCurrentThread(),
              "Session kill token destroyed while its own partition is held, which would deadlock. "
              "A token must be moved out of a scan callback rather than dropped in it.");

    Partition::Locked partition(partitionToLock);

    // A session with an outstanding kill cannot be reaped, see
    // 'ObservableSession::_shouldBeReaped'.
    auto sri = _getSessionRuntimeInfo(partition, lsid);
    invariant(sri, "Session with an outstanding kill disappeared from the catalog");

    LOGV2_DEBUG(11840100,
                2,
                "Returning an unconsumed session kill token",
                "lsid"_attr = lsid,
                "killsRequested"_attr = sri->killsRequested);

    _returnKill(partition, sri, killToken);
}

SessionCatalog::ScopedCheckedOutSession SessionCatalog::_checkOutSessionInner(
    OperationContext* opCtx,
    const LogicalSessionId& lsid,
    boost::optional<KillToken> killToken,
    Date_t deadline) {
    if (killToken) {
        dassert(killToken->lsidToKill() == lsid);
    } else {
        dassert(opCtx->getLogicalSessionId() == lsid);
    }

    auto partition = _lockPartition(lsid);

    auto sri = _getOrCreateSessionRuntimeInfo(partition, lsid);
    auto session = sri->getSession(partition, lsid);
    invariant(session);

    if (killToken) {
        invariant(ObservableSession(partition, sri, session)._killed());
    }

    // Wait until the session is no longer checked out and until the previously scheduled kill has
    // completed.
    ++session->_numWaitingToCheckOut;
    ON_BLOCK_EXIT([&] { --session->_numWaitingToCheckOut; });

    if (MONGO_unlikely(hangAfterIncrementingNumWaitingToCheckOut.shouldFail())) {
        partition.lock().unlock();
        hangAfterIncrementingNumWaitingToCheckOut.pauseWhileSet(opCtx);
        partition.lock().lock();
    }
    // '~KillToken' would return the kill anyway; doing it here reuses the held partition lock.
    const auto returnKill = [&] {
        if (killToken) {
            _returnKill(partition, sri, *killToken);
        }
    };

    const auto ok = [&] {
        try {
            return opCtx->waitForConditionOrInterruptUntil(
                sri->availableCondVar,
                partition.lock(),
                deadline,
                [&partition, &sri, &session, forKill = killToken.has_value()]() {
                    ObservableSession osession(partition, sri, session);
                    return osession._isAvailableForCheckOut(forKill);
                });
        } catch (const DBException&) {
            returnKill();
            throw;
        }
    }();

    if (!ok) {
        returnKill();
    }
    iassert(opCtx->getTimeoutError(), "operation exceeded time limit", ok);

    sri->checkoutOpCtx = opCtx;
    sri->lastCheckout = Date_t::now();

    return ScopedCheckedOutSession(*this, std::move(sri), session, std::move(killToken));
}

SessionCatalog::ScopedCheckedOutSession SessionCatalog::_checkOutSession(OperationContext* opCtx) {
    // This method is not supposed to be called with an already checked-out session due to risk of
    // deadlock
    invariant(opCtx->getLogicalSessionId());
    invariant(!operationSessionDecoration(opCtx));
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());

    auto lsid = *opCtx->getLogicalSessionId();
    return _checkOutSessionInner(opCtx, lsid, boost::none /* killToken */);
}

SessionCatalog::SessionToKill SessionCatalog::checkOutSessionForKill(OperationContext* opCtx,
                                                                     KillToken killToken,
                                                                     Date_t deadline) {
    // This method is not supposed to be called with an already checked-out session due to risk of
    // deadlock
    invariant(!operationSessionDecoration(opCtx));
    invariant(!opCtx->getTxnNumber());

    auto lsid = killToken.lsidToKill();
    return SessionToKill(_checkOutSessionInner(opCtx, lsid, std::move(killToken), deadline));
}

void SessionCatalog::scanSession(const LogicalSessionId& lsid,
                                 const ScanSessionsCallbackFn& perSessionScanFn,
                                 ScanSessionCreateSession createSession) {
    auto partition = _lockPartition(lsid);

    auto sri = (createSession == ScanSessionCreateSession::kYes)
        ? _getOrCreateSessionRuntimeInfo(partition, lsid)
        : _getSessionRuntimeInfo(partition, lsid);

    if (sri) {
        auto session = sri->getSession(partition, lsid);
        invariant(session);

        ObservableSession osession(partition, sri, session);
        perSessionScanFn(osession);
        invariant(!osession._markedForReap, "Cannot reap a session via 'scanSession'");
    }
}

std::vector<SessionCatalog::KillToken> SessionCatalog::killSessions(
    const SessionKiller::Matcher& matcher,
    ErrorCodes::Error reason,
    const KillSessionsPredicateFn& shouldKill,
    const ScanSessionsReadOnlyCallbackFn& perSessionScanFn) {
    std::vector<KillToken> killTokens;

    LOGV2_DEBUG(21976, 2, "Scanning sessions", "sessionCount"_attr = size());

    for (auto& partition : _partitions) {
        Partition::Locked locked(partition);

        for (auto& [parentLsid, sri] : locked.sessions()) {
            if (matcher.match(parentLsid)) {
                ObservableSession osession(locked, sri.get(), &sri->parentSession);
                if (perSessionScanFn) {
                    perSessionScanFn(osession);
                }
                if (!shouldKill || shouldKill(osession)) {
                    killTokens.emplace_back(osession.kill(reason));
                }
            }

            for (auto& [childLsid, session] : sri->childSessions) {
                if (matcher.match(childLsid)) {
                    ObservableSession osession(locked, sri.get(), &session);
                    if (perSessionScanFn) {
                        perSessionScanFn(osession);
                    }
                    if (!shouldKill || shouldKill(osession)) {
                        killTokens.emplace_back(osession.kill(reason));
                    }
                }
            }
        }
    }

    return killTokens;
}

LogicalSessionIdSet SessionCatalog::findExpiredParentSessions(Date_t threshold) const {
    LogicalSessionIdSet result;

    for (const auto& partition : _partitions) {
        Partition::ConstLocked locked(partition);

        for (const auto& [parentLsid, sri] : locked.sessions()) {
            if (sri->lastCheckout < threshold) {
                result.insert(parentLsid);
            }
        }
    }

    return result;
}

void SessionCatalog::scanSessions(const SessionKiller::Matcher& matcher,
                                  const ScanSessionsReadOnlyCallbackFn& workerFn) {
    LOGV2_DEBUG(6685000, 2, "Scanning sessions", "sessionCount"_attr = size());

    for (auto& partition : _partitions) {
        Partition::Locked locked(partition);

        for (auto& [parentLsid, sri] : locked.sessions()) {
            if (matcher.match(parentLsid)) {
                ObservableSession osession(locked, sri.get(), &sri->parentSession);
                workerFn(osession);
            }

            for (auto& [childLsid, session] : sri->childSessions) {
                if (matcher.match(childLsid)) {
                    ObservableSession osession(locked, sri.get(), &session);
                    workerFn(osession);
                }
            }
        }
    }
}

LogicalSessionIdSet SessionCatalog::scanSessionsForReap(
    const LogicalSessionId& parentLsid,
    const ScanSessionsCallbackFn& parentSessionWorkerFn,
    const ScanSessionsCallbackFn& childSessionWorkerFn) {
    invariant(isParentSessionId(parentLsid));

    std::unique_ptr<SessionRuntimeInfo> sriToReap;
    {
        auto partition = _lockPartition(parentLsid);

        auto sriIt = partition.sessions().find(parentLsid);
        // The reaper should never try to reap a non-existent session id.
        invariant(sriIt != partition.sessions().end());
        auto sri = sriIt->second.get();

        LogicalSessionIdSet remainingSessions;
        bool shouldReapRemaining = true;

        {
            ObservableSession osession(partition, sri, &sri->parentSession);
            parentSessionWorkerFn(osession);

            remainingSessions.insert(osession.getSessionId());
            shouldReapRemaining = osession._shouldBeReaped();
        }

        {
            auto childSessionIt = sri->childSessions.begin();
            while (childSessionIt != sri->childSessions.end()) {
                ObservableSession osession(partition, sri, &childSessionIt->second);
                childSessionWorkerFn(osession);

                if (osession._shouldBeReaped() &&
                    (osession._reapMode == ObservableSession::ReapMode::kExclusive)) {
                    sri->childSessions.erase(childSessionIt++);
                    continue;
                }

                remainingSessions.insert(osession.getSessionId());
                shouldReapRemaining &= osession._shouldBeReaped();
                ++childSessionIt;
            }
        }

        if (shouldReapRemaining) {
            sriToReap = std::move(sriIt->second);
            partition.sessions().erase(sriIt);
            _numParentSessions.fetchAndSubtract(1);
            remainingSessions.clear();
        }

        return remainingSessions;
    }
}

boost::optional<SessionCatalog::KillToken> SessionCatalog::killSessionIf(
    const LogicalSessionId& lsid,
    const KillSessionsPredicateFn& shouldKill,
    ErrorCodes::Error reason) {
    // Declared out here so that the token outlives the partition lock below: returning a kill needs
    // that same partition.
    boost::optional<KillToken> killToken;

    {
        auto partition = _lockPartition(lsid);

        if (auto sri = _getSessionRuntimeInfo(partition, lsid)) {
            auto session = sri->getSession(partition, lsid);
            invariant(session);

            ObservableSession osession(partition, sri, session);
            if (shouldKill(osession)) {
                killToken.emplace(osession.kill(reason));
            }
        }
    }

    return killToken;
}

SessionCatalog::KillToken SessionCatalog::killSession(const LogicalSessionId& lsid,
                                                      ErrorCodes::Error reason) {
    auto partition = _lockPartition(lsid);

    auto sri = _getSessionRuntimeInfo(partition, lsid);
    uassert(ErrorCodes::NoSuchSession, "Session not found", sri);
    auto session = sri->getSession(partition, lsid);
    uassert(ErrorCodes::NoSuchSession, "Session not found", session);
    return ObservableSession(partition, sri, session).kill(reason);
}

size_t SessionCatalog::size() const {
    return static_cast<size_t>(_numParentSessions.load());
}

size_t SessionCatalog::numSessionsWithOutstandingKills() const {
    return static_cast<size_t>(_numSessionsWithOutstandingKills.load());
}

void SessionCatalog::SessionRuntimeInfo::returnKill(WithLock) {
    invariant(killsRequested > 0);
    if (--killsRequested == 0) {
        catalog->_numSessionsWithOutstandingKills.fetchAndSubtract(1);
    }
    availableCondVar.notify_all();
}

void SessionCatalog::setDisallowNewTransactions() {
    _disallowNewTransactions.store(true);
}

bool SessionCatalog::getDisallowNewTransactions() {
    return _disallowNewTransactions.load();
}

SessionCatalog::Partition& SessionCatalog::_partitionFor(const LogicalSessionId& lsid) {
    // Hash only the 'id' component, which parent and child sessions share, so a whole session
    // family maps to one partition without materializing the parent lsid.
    return _partitions[UUID::Hash{}(lsid.getId()) % _partitions.size()];
}

SessionCatalog::Partition::Locked SessionCatalog::_lockPartition(const LogicalSessionId& lsid) {
    return Partition::Locked(_partitionFor(lsid));
}

SessionCatalog::SessionRuntimeInfo* SessionCatalog::_getSessionRuntimeInfo(
    const Partition::Locked& partition, const LogicalSessionId& lsid) {
    const auto& parentLsid = isParentSessionId(lsid) ? lsid : *getParentSessionId(lsid);
    auto sriIt = partition.sessions().find(parentLsid);

    if (sriIt == partition.sessions().end()) {
        return nullptr;
    }

    auto sri = sriIt->second.get();
    auto session = sri->getSession(partition, lsid);

    if (session) {
        return sri;
    }

    return nullptr;
}

SessionCatalog::SessionRuntimeInfo* SessionCatalog::_getOrCreateSessionRuntimeInfo(
    const Partition::Locked& partition, const LogicalSessionId& lsid) {
    if (auto sri = _getSessionRuntimeInfo(partition, lsid)) {
        return sri;
    }

    const auto& parentLsid = isParentSessionId(lsid) ? lsid : *getParentSessionId(lsid);
    // The parent entry may already exist, so only count an insertion that took place.
    auto [sriIt, inserted] = partition.sessions().emplace(
        parentLsid, std::make_unique<SessionRuntimeInfo>(this, parentLsid));
    if (inserted) {
        _numParentSessions.fetchAndAdd(1);
    }
    auto sri = sriIt->second.get();

    if (isChildSession(lsid)) {
        auto [childSessionIt, inserted] = sri->childSessions.try_emplace(lsid, lsid);
        // Insert should always succeed since the session did not exist prior to this.
        invariant(inserted);

        auto& childSession = childSessionIt->second;
        childSession._parentSession = &sri->parentSession;
    }

    return sri;
}

void SessionCatalog::_releaseSession(
    SessionRuntimeInfo* sri,
    Session* session,
    boost::optional<KillToken> killToken,
    boost::optional<TxnNumberAndProvenance> clientTxnNumberStarted) {
    ServiceContext* service = nullptr;
    std::vector<LogicalSessionId> eagerlyReapedSessions;

    // The partition must be unlocked before invoking the eager reap callback below.
    {
        auto partition = _lockPartition(sri->parentSession.getSessionId());

        // Make sure we have exactly the same session on the map and that it is still associated
        // with an operation context (meaning checked-out)
        auto sriIt = partition.sessions().find(sri->parentSession.getSessionId());
        invariant(sriIt != partition.sessions().end());
        invariant(sriIt->second.get() == sri);
        invariant(sri->checkoutOpCtx);
        if (killToken) {
            dassert(killToken->lsidToKill() == session->getSessionId());
        }

        service = sri->checkoutOpCtx->getServiceContext();

        sri->checkoutOpCtx = nullptr;
        sri->availableCondVar.notify_all();

        if (killToken) {
            _returnKill(partition, sri, *killToken);
        }

        if (clientTxnNumberStarted.has_value()) {
            auto [txnNumber, provenance] = *clientTxnNumberStarted;

            // Since the given txnNumber successfully started, we know any child sessions with
            // older txnNumbers can be discarded. This needed to wait until a transaction started
            // because that can fail, e.g. if the active transaction is prepared.
            auto workerFn = _makeSessionWorkerFnForEagerReap(service, txnNumber, provenance);
            auto numReaped = stdx::erase_if(sri->childSessions, [&](auto&& it) {
                ObservableSession osession(partition, sri, &it.second);
                workerFn(osession);

                bool willReap = osession._shouldBeReaped() &&
                    (osession._reapMode == ObservableSession::ReapMode::kExclusive);
                if (willReap) {
                    eagerlyReapedSessions.push_back(std::move(it.first));
                }
                return willReap;
            });

            sri->lastClientTxnNumberStarted = txnNumber;

            LOGV2_DEBUG(6685200,
                        4,
                        "Erased child sessions",
                        "releasedLsid"_attr = session->getSessionId(),
                        "clientTxnNumber"_attr = txnNumber,
                        "childSessionsRemaining"_attr = sri->childSessions.size(),
                        "numReaped"_attr = numReaped,
                        "provenance"_attr = provenanceToString(provenance));
        }
    }

    if (eagerlyReapedSessions.size() && _onEagerlyReapedSessionsFn) {
        (*_onEagerlyReapedSessionsFn)(service, std::move(eagerlyReapedSessions));
    }
}

SessionCatalog::ScanSessionsCallbackFn SessionCatalog::_defaultMakeSessionWorkerFnForEagerReap(
    ServiceContext* service, TxnNumber clientTxnNumberStarted, Provenance provenance) {
    return [clientTxnNumberStarted](ObservableSession& osession) {
        // If a higher txnNumber has been seen for a client and started a transaction, assume any
        // child sessions for lower transactions have been superseded and can be reaped.
        const auto& transactionSessionId = osession.getSessionId();
        if (transactionSessionId.getTxnNumber() &&
            *transactionSessionId.getTxnNumber() < clientTxnNumberStarted) {
            osession.markForReap(ObservableSession::ReapMode::kExclusive);
        }
    };
}

Session* SessionCatalog::SessionRuntimeInfo::getSession(WithLock, const LogicalSessionId& lsid) {
    if (isParentSessionId(lsid)) {
        // We should have already compared the parent lsid when we found this SRI.
        dassert(lsid == parentSession._sessionId);
        return &parentSession;
    }

    dassert(getParentSessionId(lsid) == parentSession._sessionId);
    auto it = childSessions.find(lsid);
    if (it == childSessions.end()) {
        return nullptr;
    }
    return &it->second;
}

SessionCatalog::KillToken ObservableSession::kill(ErrorCodes::Error reason) const {
    const bool firstKiller = (0 == _sri->killsRequested);
    ++_sri->killsRequested;
    if (firstKiller) {
        _sri->catalog->_numSessionsWithOutstandingKills.fetchAndAdd(1);
    }

    if (firstKiller && hasCurrentOperation()) {
        const auto serviceContext = _sri->checkoutOpCtx->getServiceContext();
        serviceContext->killOperation(_clientLock, _sri->checkoutOpCtx, reason);
    }

    return SessionCatalog::KillToken(_sri->catalog, getSessionId());
}

void ObservableSession::markForReap(ReapMode reapMode) {
    if (isParentSessionId(getSessionId())) {
        // By design, parent sessions are only safe to be reaped if all of their child sessions are.
        invariant(reapMode == ReapMode::kNonExclusive);
    }
    _markedForReap = true;
    _reapMode.emplace(reapMode);
}

bool ObservableSession::_shouldBeReaped() const {
    bool isCheckedOut = [&] {
        if (_sri->checkoutOpCtx) {
            return _sri->checkoutOpCtx->getLogicalSessionId() == getSessionId();
        }
        return false;
    }();
    return _markedForReap && !isCheckedOut && !get()->_numWaitingToCheckOut && !_killed();
}

bool ObservableSession::_killed() const {
    return _sri->killsRequested > 0;
}

OperationContextSession::OperationContextSession(OperationContext* opCtx) : _opCtx(opCtx) {
    auto& checkedOutSession = operationSessionDecoration(opCtx);
    if (checkedOutSession) {
        // The only case where a session can be checked-out more than once is due to DBDirectClient
        // reentrancy
        invariant(opCtx->getClient()->isInDirectClient());
        return;
    }

    checkOut(opCtx);
}

OperationContextSession::OperationContextSession(OperationContext* opCtx,
                                                 SessionCatalog::KillToken killToken)
    : _opCtx(opCtx) {
    auto& checkedOutSession = operationSessionDecoration(opCtx);

    invariant(!checkedOutSession);
    invariant(!opCtx->getLogicalSessionId());  // lsid is specified by killToken argument.

    const auto catalog = SessionCatalog::get(opCtx);
    auto scopedSessionForKill = catalog->checkOutSessionForKill(opCtx, std::move(killToken));

    // We acquire a Client lock here to guard the construction of this session so that references to
    // this session are safe to use while the lock is held
    std::lock_guard lk(*opCtx->getClient());
    checkedOutSession.emplace(std::move(scopedSessionForKill._scos));
}

OperationContextSession::~OperationContextSession() {
    // Only release the checked out session at the end of the top-level request from the client, not
    // at the end of a nested DBDirectClient call
    if (_opCtx->getClient()->isInDirectClient()) {
        return;
    }

    auto& checkedOutSession = operationSessionDecoration(_opCtx);
    if (!checkedOutSession)
        return;

    checkIn(_opCtx, CheckInReason::kDone);
}

Session* OperationContextSession::get(OperationContext* opCtx) {
    auto& checkedOutSession = operationSessionDecoration(opCtx);
    if (checkedOutSession) {
        return checkedOutSession->get();
    }

    return nullptr;
}

void OperationContextSession::checkIn(OperationContext* opCtx, CheckInReason reason) {
    auto& checkedOutSession = operationSessionDecoration(opCtx);
    invariant(checkedOutSession);

    if (reason == CheckInReason::kYield) {
        // Don't allow yielding a session that was checked out for kill because it will "unkill" the
        // session and the subsequent check out will not have priority, which can easily lead to
        // bugs. If you need to run an operation with a session that may yield, kill the session,
        // check it out for kill, release it, then check it out normally.
        invariant(!checkedOutSession->wasCheckedOutForKill());
    }

    // Removing the checkedOutSession from the OperationContext must be done under the Client lock,
    // but destruction of the checkedOutSession must not be, as it takes the SessionCatalog mutex,
    // and other code may take the Client lock while holding that mutex.
    std::unique_lock<Client> lk(*opCtx->getClient());
    SessionCatalog::ScopedCheckedOutSession sessionToReleaseOutOfLock(
        std::move(*checkedOutSession));

    // This destroys the moved-from ScopedCheckedOutSession, and must be done within the client lock
    checkedOutSession = boost::none;
    lk.unlock();
}

void OperationContextSession::checkOut(OperationContext* opCtx) {
    auto& checkedOutSession = operationSessionDecoration(opCtx);
    invariant(!checkedOutSession);

    const auto catalog = SessionCatalog::get(opCtx);
    auto scopedCheckedOutSession = catalog->_checkOutSession(opCtx);

    // We acquire a Client lock here to guard the construction of this session so that references to
    // this session are safe to use while the lock is held
    std::lock_guard<Client> lk(*opCtx->getClient());
    checkedOutSession.emplace(std::move(scopedCheckedOutSession));
}

void OperationContextSession::observeNewTxnNumberStarted(
    OperationContext* opCtx,
    const LogicalSessionId& lsid,
    SessionCatalog::TxnNumberAndProvenance txnNumberAndProvenance) {
    auto& checkedOutSession = operationSessionDecoration(opCtx);
    invariant(checkedOutSession);

    LOGV2_DEBUG(6685201,
                4,
                "Observing new retryable write number started on session",
                "lsid"_attr = lsid,
                "txnNumber"_attr = txnNumberAndProvenance.first,
                "provenance"_attr = txnNumberAndProvenance.second);

    const auto& checkedOutLsid = (*checkedOutSession)->getSessionId();
    if (isParentSessionId(lsid)) {
        // Observing a new transaction/retryable write on a parent session.

        // The operation must have checked out the parent session itself or a child session of the
        // parent. This is safe because both share the same SessionRuntimeInfo.
        dassert(lsid == checkedOutLsid || lsid == *getParentSessionId(checkedOutLsid));

        checkedOutSession->observeNewClientTxnNumberStarted(txnNumberAndProvenance);
    } else if (isInternalSessionForRetryableWrite(lsid)) {
        // Observing a new internal transaction on a retryable session.

        // A transaction on a child session is always begun on an operation that checked it out
        // directly.
        dassert(lsid == checkedOutLsid);

        checkedOutSession->observeNewClientTxnNumberStarted(
            {*lsid.getTxnNumber(), txnNumberAndProvenance.second});
    }
}

}  // namespace mongo
