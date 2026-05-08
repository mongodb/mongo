/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/repl/intent_registry.h"

#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/decorable.h"

#include <mutex>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {

namespace rss {
namespace consensus {

// Decoration on Client. When a client is destroyed (e.g., connection closed) while its stashed
// WUOW still holds write-intent tokens, removes those tokens before the Client's memory is freed.
// This ensures _killOperationsByIntent cannot call ClientLock on a deleted object.
//
// Limitation: C++ destroys Client's own data members (including _desc) before running
// Decorable<Client> base-class destructors, so this decoration's destructor runs after _desc is
// already freed. There is therefore a window where a token still exists in the map but
// client->desc() would touch freed memory. TokenMapEntry::clientDesc stores the description at
// registration time so that all logging in _killOperationsByIntent and _waitForDrain uses the
// snapshot value rather than dereferencing the Client after its destruction has begun.
class ClientIntentCleanup {
public:
    void init(IntentRegistry* registry, Client* client) {
        if (!_registry) {
            _registry = registry;
            _client = client;
        }
    }

    ~ClientIntentCleanup() {
        if (_registry) {
            _registry->deregisterTokensForClient(_client);
        }
    }

private:
    IntentRegistry* _registry = nullptr;
    Client* _client = nullptr;
};

// Decoration on OperationContext. When an opCtx with active write intents is destroyed (e.g.,
// within a multi-document transaction where the lock release is deferred to the WUOW end),
// removes the opId-to-counter mapping from IntentRegistry so the deferred deregistration
// callback skips decrementing the counter of whatever new opCtx is at the same memory address.
class WriteIntentCleanup {
public:
    void init(IntentRegistry* registry, uint64_t opId) {
        if (!_registry) {
            _registry = registry;
            _opId = opId;
        }
    }

    ~WriteIntentCleanup() {
        if (_registry) {
            _registry->_unregisterWriteCountForOpId(_opId);
        }
    }

private:
    IntentRegistry* _registry = nullptr;
    uint64_t _opId = 0;
};

namespace {

auto registryDecoration = ServiceContext::declareDecoration<IntentRegistry>();
const auto writeIntentCountOnOpCtx = OperationContext::declareDecoration<AtomicWord<int32_t>>();
// Declared after writeIntentCountOnOpCtx so it destructs first (reverse order), ensuring the
// counter pointer is removed from the registry before the counter memory is freed.
const auto writeIntentCleanup = OperationContext::declareDecoration<WriteIntentCleanup>();
const auto clientIntentCleanup = Client::declareDecoration<ClientIntentCleanup>();

}  // namespace

using namespace std::chrono_literals;

// Tracks the number of operations killed by intent registry on state transition.
auto& totalOpsKilledByIntentRegistry =
    *MetricBuilder<Counter64>("repl.stateTransition.totalOperationsKilledByIntentRegistry");

IntentRegistry::IntentToken::IntentToken(Intent intent) : _intent(intent) {
    _id = _currentTokenId.fetchAndAdd(1);
}
IntentRegistry::IntentToken::idType IntentRegistry::IntentToken::id() const {
    return _id;
}

IntentRegistry::Intent IntentRegistry::IntentToken::intent() const {
    return _intent;
}

IntentRegistry::IntentRegistry()
    : _tokenMaps((size_t)IntentRegistry::Intent::_NumDistinctIntents_) {}

IntentRegistry& IntentRegistry::get(ServiceContext* serviceContext) {
    return registryDecoration(serviceContext);
}

IntentRegistry& IntentRegistry::get(OperationContext* opCtx) {
    return get(opCtx->getClient()->getServiceContext());
}

IntentRegistry::IntentToken IntentRegistry::registerIntent(IntentRegistry::Intent intent,
                                                           OperationContext* opCtx) {
    invariant(intent < Intent::_NumDistinctIntents_);
    invariant(opCtx);

    // Check these outside of the mutex to avoid a deadlock against the Replication Coordinator
    // mutex.
    bool isReplSet =
        repl::ReplicationCoordinator::get(opCtx->getServiceContext())->getSettings().isReplSet();

    std::shared_lock lock(_stateMutex);

    // Do not interrupt if the killing opCtx is performing work or we are inside an
    // UninterruptibleLockGuard.
    if (opCtx != _interruptionCtx &&
        !opCtx->uninterruptibleLocksRequested_DO_NOT_USE()) {  // NOLINT
        auto validIntent = _validIntent(intent);
        if (_lastInterruption == InterruptionType::Shutdown) {
            if (!validIntent) {
                // Mark opCtx as killed before uasserting.
                auto serviceCtx = opCtx->getServiceContext();
                auto client = opCtx->getClient();
                ClientLock cl_lock(client);
                serviceCtx->killOperation(cl_lock, opCtx, ErrorCodes::InterruptedAtShutdown);
            }
            uassert(
                ErrorCodes::InterruptedAtShutdown,
                fmt::format("Cannot register {} intent due to Shutdown.", intentToString(intent)),
                validIntent);
        } else {
            uassert(ErrorCodes::InterruptedDueToReplStateChange,
                    fmt::format("Cannot register {} intent due to ReplStateChange.",
                                intentToString(intent)),
                    validIntent);
        }

        if (isReplSet && intent == Intent::Write) {
            // canAcceptWritesFor asserts that we have the RSTL acquired.
            bool isWritablePrimary =
                repl::ReplicationCoordinator::get(opCtx->getServiceContext())
                    ->canAcceptWritesFor_UNSAFE(opCtx, NamespaceString(DatabaseName::kAdmin));
            uassert(ErrorCodes::NotWritablePrimary,
                    "Cannot register write intent if we are not primary.",
                    isWritablePrimary);
        }
    }

    auto& tokenMap = _tokenMaps[(size_t)intent];
    if (intent == Intent::BlockingWrite) {
        if (opCtx->uninterruptibleLocksRequested_DO_NOT_USE()) {  // NOLINT
            // Do not check for interrupt if we are in an uninterruptible lock guard.
            _pendingStateChangeCV.wait(lock, [&] { return _pendingStateChange.load() == 0; });
        } else {
            opCtx->waitForConditionOrInterrupt(
                _pendingStateChangeCV, lock, [&] { return _pendingStateChange.load() == 0; });
        }
    }

    IntentToken token(intent);
    if (intent == Intent::Write || intent == Intent::BlockingWrite) {
        auto newCount = writeIntentCountOnOpCtx(opCtx).addAndFetch(1);
        LOGV2_DEBUG(12436500,
                    3,
                    "Register Intent",
                    "token"_attr = token.id(),
                    "intent"_attr = intentToString(intent),
                    "opCtx"_attr = opCtx->getOpID(),
                    "writeIntentCount"_attr = newCount);
        // Register the opId -> counter mapping so deregisterIntent can correctly
        // skip the decrement if this opCtx is destroyed before the WUOW ends.
        {
            std::lock_guard opIdLock(_opIdMutex);
            _opIdToWriteCountPtr.try_emplace(opCtx->getOpID(), &writeIntentCountOnOpCtx(opCtx));
        }
        writeIntentCleanup(opCtx).init(this, opCtx->getOpID());
        clientIntentCleanup(opCtx->getClient()).init(this, opCtx->getClient());
    } else {
        LOGV2_DEBUG(12436501,
                    3,
                    "Register Intent",
                    "token"_attr = token.id(),
                    "intent"_attr = intentToString(intent),
                    "opCtx"_attr = opCtx->getOpID());
    }
    {
        std::unique_lock<std::mutex> lockTokenMap(tokenMap.lock);
        tokenMap.map.insert({token.id(),
                             {opCtx,
                              opCtx->getClient(),
                              opCtx->getServiceContext(),
                              opCtx->getOpID(),
                              opCtx->getLogicalSessionId(),
                              opCtx->getClient()->desc()}});
    }

    return token;
}
void IntentRegistry::deregisterIntent(IntentRegistry::IntentToken token) {
    auto& tokenMap = _tokenMaps[(size_t)token.intent()];
    std::lock_guard<std::mutex> lock(tokenMap.lock);

    if (token.intent() == Intent::Write || token.intent() == Intent::BlockingWrite) {
        auto it = tokenMap.map.find(token.id());
        if (it != tokenMap.map.end()) {
            const uint64_t opId = it->second.opId;
            // Use the opId (captured at registration time) to find the counter. If the opCtx was
            // destroyed before this callback fired (e.g., deferred lock release in a multi-document
            // transaction), WriteIntentCleanup will have already removed the entry, and we skip
            // the decrement to avoid corrupting a new opCtx allocated at the same address.
            std::lock_guard opIdLock(_opIdMutex);
            auto countIt = _opIdToWriteCountPtr.find(opId);
            if (countIt != _opIdToWriteCountPtr.end()) {
                auto newCount = countIt->second->subtractAndFetch(1);
                LOGV2_DEBUG(12436502,
                            3,
                            "Deregister Intent",
                            "token"_attr = token.id(),
                            "intent"_attr = intentToString(token.intent()),
                            "opId"_attr = opId,
                            "writeIntentCount"_attr = newCount);
                if (newCount == 0) {
                    _opIdToWriteCountPtr.erase(countIt);
                }
            } else {
                LOGV2_DEBUG(12436503,
                            3,
                            "Deregister Intent skipped (opCtx already destroyed)",
                            "token"_attr = token.id(),
                            "intent"_attr = intentToString(token.intent()),
                            "opId"_attr = opId);
            }
        }
    }
    (void)tokenMap.map.erase(token.id());
    if (tokenMap.map.empty()) {
        tokenMap.cv.notify_all();
    }
}

void IntentRegistry::_unregisterWriteCountForOpId(uint64_t opId) {
    std::lock_guard opIdLock(_opIdMutex);
    _opIdToWriteCountPtr.erase(opId);
}

void IntentRegistry::deregisterTokensForClient(Client* client) {
    for (size_t i = 0; i < _tokenMaps.size(); i++) {
        auto intent = static_cast<Intent>(i);
        auto& tokenMap = _tokenMaps[i];

        std::vector<IntentToken> tokensToDeregister;
        {
            std::lock_guard<std::mutex> lock(tokenMap.lock);
            for (auto& [id, entry] : tokenMap.map) {
                if (entry.client == client) {
                    tokensToDeregister.push_back(IntentToken(intent, id));
                }
            }
        }

        for (auto& token : tokensToDeregister) {
            deregisterIntent(token);
        }
    }
}

void IntentRegistry::deregisterTokensForSession(OperationContext* opCtx,
                                                const LogicalSessionId& lsid) {
    // Match on either the child lsid directly or its parent lsid for internal transactions.
    auto parentLsid = getParentSessionId(lsid);
    uint64_t currentOpId = opCtx->getOpID();

    for (size_t i = 0; i < _tokenMaps.size(); i++) {
        auto intent = static_cast<Intent>(i);
        auto& tokenMap = _tokenMaps[i];

        std::vector<IntentToken> tokensToDeregister;
        {
            std::lock_guard<std::mutex> lock(tokenMap.lock);
            for (auto& [id, entry] : tokenMap.map) {
                bool lsidMatch = entry.lsid &&
                    (*entry.lsid == lsid || (parentLsid && *entry.lsid == *parentLsid));
                bool opIdMatch = entry.opId == currentOpId;
                if (lsidMatch || opIdMatch) {
                    tokensToDeregister.push_back(IntentToken(intent, id));
                }
            }
        }

        for (auto& token : tokensToDeregister) {
            deregisterIntent(token);
        }
    }
}

bool IntentRegistry::canDeclareIntent(Intent intent, OperationContext* opCtx) {
    invariant(intent < Intent::_NumDistinctIntents_);
    invariant(opCtx);

    // Check these outside of the mutex to avoid a deadlock against the Replication Coordinator
    // mutex.
    bool isReplSet =
        repl::ReplicationCoordinator::get(opCtx->getServiceContext())->getSettings().isReplSet();

    std::shared_lock lock(_stateMutex);

    if (opCtx != _interruptionCtx) {
        if (!_validIntent(intent)) {
            return false;
        }

        if (isReplSet && intent == Intent::Write) {
            // canAcceptWritesFor asserts that we have the RSTL acquired.
            return repl::ReplicationCoordinator::get(opCtx->getServiceContext())
                ->canAcceptWritesFor_UNSAFE(opCtx, NamespaceString(DatabaseName::kAdmin));
        }
    }

    return true;
}

std::future<ReplicationStateTransitionGuard> IntentRegistry::killConflictingOperations(
    IntentRegistry::InterruptionType interrupt,
    OperationContext* opCtx,
    std::function<void()> postInterruptionCallback,
    boost::optional<uint32_t> timeout_sec) {
    LOGV2(9945003, "Intent Registry killConflictingOperations", "interrupt"_attr = interrupt);
    _pendingStateChange.fetchAndAdd(1);
    auto timeOutSec = std::chrono::seconds(
        timeout_sec ? *timeout_sec : repl::fassertOnLockTimeoutForStepUpDown.load());

    _waitForDrain(Intent::BlockingWrite,
                  std::chrono::duration_cast<std::chrono::milliseconds>(timeOutSec),
                  interrupt);
    {
        std::unique_lock lock(_stateMutex);
        if (_interruptionCtx) {
            LOGV2(9945001, "Existing kill ongoing. Blocking until it is finished.");
        }

        _activeInterruptionCV.wait(lock, [this] { return !_interruptionCtx; });
        _lastInterruption = interrupt;
        _interruptionCtx = opCtx;
    }

    //  NOLINTNEXTLINE
    return std::async(
        std::launch::async, [&, interrupt, timeOutSec, cb = std::move(postInterruptionCallback)] {
            const std::vector<Intent>* intents = nullptr;
            switch (interrupt) {
                case InterruptionType::Rollback: {
                    static const std::vector<Intent> rollbackIntents = {Intent::Write,
                                                                        Intent::Read};
                    intents = &rollbackIntents;
                } break;
                case InterruptionType::Shutdown: {
                    static const std::vector<Intent> shutdownIntents = {
                        Intent::Write, Intent::Read, Intent::LocalWrite};
                    intents = &shutdownIntents;
                } break;
                case InterruptionType::StepDown: {
                    static const std::vector<Intent> stepdownIntents = {Intent::Write};
                    intents = &stepdownIntents;
                } break;
                case InterruptionType::StepUp:
                    break;
                default:
                    break;
            }

            if (intents) {
                for (auto intent : *intents) {
                    _killOperationsByIntent(intent, interrupt);
                }

                if (cb) {
                    try {
                        cb();
                    } catch (const DBException& e) {
                        LOGV2_WARNING(12436505,
                                      "postInterruptionCallback threw during intent drain",
                                      "error"_attr = e.toStatus());
                    }
                }

                Timer timer;
                auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(timeOutSec);
                for (auto intent : *intents) {
                    _waitForDrain(intent, timeout, interrupt);
                    // Negative duration to cv::wait_for can cause undefined behavior
                    // Since timeout == 0 is a special case to enable untimed wait we prevent a
                    // non-zero timeout to ever drop to 0 by setting it to at least to 1ms
                    if (timeout.count()) {
                        timeout -= std::min(
                            std::chrono::milliseconds(durationCount<Milliseconds>(timer.elapsed())),
                            timeout - 1ms);
                    }
                }
            }

            updateAndLogStateTransitionMetrics(interrupt, _totalOpsKilled);
            _totalOpsKilled = 0;

            return ReplicationStateTransitionGuard([&]() {
                std::lock_guard lock(_stateMutex);
                _interruptionCtx = nullptr;
                _lastInterruption = InterruptionType::None;
                _activeInterruptionCV.notify_one();
                if (_pendingStateChange.subtractAndFetch(1) == 0) {
                    _pendingStateChangeCV.notify_all();
                }
            });
        });
}

void IntentRegistry::updateAndLogStateTransitionMetrics(IntentRegistry::InterruptionType interrupt,
                                                        size_t numOpsKilled) const {
    // Clear the current metrics before setting.
    totalOpsKilledByIntentRegistry.decrement(totalOpsKilledByIntentRegistry.get());

    totalOpsKilledByIntentRegistry.increment(numOpsKilled);

    BSONObjBuilder bob;
    bob.append("lastStateTransition", interruptionToString(interrupt));
    bob.appendNumber("totalOpsKilledByIntentRegistry", totalOpsKilledByIntentRegistry.get());

    LOGV2(10286300, "State transition ops metrics for intent registry", "metrics"_attr = bob.obj());
}

bool IntentRegistry::hasWriteIntentDeclared(const OperationContext* opCtx) {
    return 0 != writeIntentCountOnOpCtx(opCtx).load();
}

void IntentRegistry::enable() {
    std::lock_guard lock(_stateMutex);
    _enabled = true;
    _lastInterruption = InterruptionType::None;
    _interruptionCtx = nullptr;
    _activeInterruptionCV.notify_one();
}

void IntentRegistry::disable() {
    std::lock_guard lock(_stateMutex);
    _enabled = false;
}

bool IntentRegistry::_validIntent(IntentRegistry::Intent intent) const {
    if (!_enabled) {
        return false;
    }
    switch (_lastInterruption) {
        case InterruptionType::Shutdown:
            return false;
        case InterruptionType::Rollback:
            return intent == Intent::LocalWrite;
        case InterruptionType::StepDown:
            return intent != Intent::Write;
        default:
            return true;
    }
}

void IntentRegistry::_killOperationsByIntent(IntentRegistry::Intent intent,
                                             InterruptionType interruption) {
    auto& tokenMap = _tokenMaps[(size_t)intent];
    std::lock_guard<std::mutex> lock(tokenMap.lock);
    for (auto& [token, entry] : tokenMap.map) {
        // Atomically look up and lock the client by opId rather than dereferencing the stored
        // Client* directly. This is safe even if the client is undergoing destruction: once
        // Client::~Client() begins, the opId lease is no longer active and getLockedClient
        // returns an empty lock. It also handles the stashed-WUOW case (original opCtx replaced
        // on the client) because the opId check inside getLockedClient will not match.
        ClientLock clientLock = entry.svcCtx->getLockedClient(static_cast<OperationId>(entry.opId));
        if (!clientLock) {
            // Client is gone, being destroyed, or is now running a different operation.
            // The postInterruptionCallback will abort any stashed transactions.
            LOGV2(12436506,
                  "Skipping intent token: client or matching opCtx no longer active",
                  "name"_attr = entry.clientDesc,
                  "registered_token"_attr = token,
                  "registered_opId"_attr = entry.opId);
            continue;
        }

        if (interruption == InterruptionType::StepDown &&
            !clientLock->canKillOperationInStepdown()) {
            LOGV2(10336502,
                  "Skipping killing intent for stepdown due to unkillable client",
                  "name"_attr = entry.clientDesc,
                  "registered_token"_attr = token);
            continue;
        }

        auto* currentOpCtx = clientLock->getOperationContext();
        if (!currentOpCtx) {
            continue;
        }

        // Do not kill opCtx's that are inside an UninterruptibleLockGuard.
        if (currentOpCtx->uninterruptibleLocksRequested_DO_NOT_USE()) {  // NOLINT
            LOGV2(10336500,
                  "Skipping killing intent due to UninterruptibleLockGuard",
                  "name"_attr = entry.clientDesc,
                  "registered_token"_attr = token);
            continue;
        }

        if (currentOpCtx->getKillStatus() != ErrorCodes::OK) {
            continue;
        }

        if (interruption == InterruptionType::Shutdown) {
            entry.svcCtx->killOperation(
                clientLock, currentOpCtx, ErrorCodes::InterruptedAtShutdown);
        } else {
            entry.svcCtx->killOperation(
                clientLock, currentOpCtx, ErrorCodes::InterruptedDueToReplStateChange);
        }
        _totalOpsKilled += 1;
        LOGV2(9795400,
              "Repl state change interrupted a thread.",
              "name"_attr = entry.clientDesc,
              "registered token"_attr = token,
              "killcode"_attr = currentOpCtx->getKillStatus());
    }
}

void IntentRegistry::_waitForDrain(IntentRegistry::Intent intent,
                                   std::chrono::milliseconds timeout,
                                   InterruptionType interruption) {
    static constexpr auto kRetryKillInterval = std::chrono::milliseconds(100);

    auto& tokenMap = _tokenMaps[(size_t)intent];
    std::unique_lock<std::mutex> lock(tokenMap.lock);

    auto logAndFassert = [&]() {
        LOGV2(
            9795403, "There are still registered intents", "Intent"_attr = intentToString(intent));
        for (auto& [token, entry] : tokenMap.map) {
            LOGV2(9795402,
                  "Registered token:",
                  "token_id"_attr = token,
                  "client"_attr = entry.clientDesc);
        }
        LOGV2_FATAL_CONTINUE(9795404,
                             "Timeout while waiting on intent queue to drain, printing stack "
                             "traces then calling abort() to allow the cluster to progress.");
#if defined(MONGO_STACKTRACE_CAN_DUMP_ALL_THREADS)
        printAllThreadStacksBlocking();
#endif
        fasserted(9795401);
    };

    if (timeout.count()) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!tokenMap.map.empty()) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                logAndFassert();
                return;
            }
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            auto waitTime = std::min(remaining, kRetryKillInterval);
            tokenMap.cv.wait_for(lock, waitTime, [&tokenMap] { return tokenMap.map.empty(); });

            if (!tokenMap.map.empty()) {
                lock.unlock();
                _killOperationsByIntent(intent, interruption);
                lock.lock();
            }
        }
    } else {
        while (!tokenMap.map.empty()) {
            tokenMap.cv.wait_for(
                lock, kRetryKillInterval, [&tokenMap] { return tokenMap.map.empty(); });
            if (!tokenMap.map.empty()) {
                lock.unlock();
                _killOperationsByIntent(intent, interruption);
                lock.lock();
            }
        }
    }
}

size_t IntentRegistry::getTotalOpsKilled() const {
    return _totalOpsKilled;
}

std::vector<size_t> IntentRegistry::getTotalIntentsDeclared() const {
    auto getTotalIntents = [&](IntentRegistry::Intent intent) {
        auto& tokenMap = _tokenMaps[(size_t)intent];
        std::unique_lock<std::mutex> lock(tokenMap.lock);
        return tokenMap.map.size();
    };
    auto res = std::vector<size_t>();
    res.emplace_back(getTotalIntents(IntentRegistry::Intent::Read));
    res.emplace_back(getTotalIntents(IntentRegistry::Intent::Write));
    res.emplace_back(getTotalIntents(IntentRegistry::Intent::LocalWrite));
    res.emplace_back(getTotalIntents(IntentRegistry::Intent::BlockingWrite));
    return res;
}

std::string IntentRegistry::intentToString(IntentRegistry::Intent intent) {
    switch (intent) {
        case Intent::LocalWrite:
            return "LOCAL_WRITE";
        case Intent::Read:
            return "READ";
        case Intent::Write:
            return "WRITE";
        case Intent::BlockingWrite:
            return "BLOCKING_WRITE";
        default:
            return "UNKNOWN";
    }
}

std::string IntentRegistry::interruptionToString(InterruptionType interrupt) {
    switch (interrupt) {
        case IntentRegistry::InterruptionType::Rollback:
            return "ROLLBACK";
        case IntentRegistry::InterruptionType::Shutdown:
            return "SHUTDOWN";
        case IntentRegistry::InterruptionType::StepUp:
            return "STEPUP";
        case IntentRegistry::InterruptionType::StepDown:
            return "STEPDOWN";
        default:
            return "UNKNOWN";
    }
}

}  // namespace consensus
}  // namespace rss
}  // namespace mongo
