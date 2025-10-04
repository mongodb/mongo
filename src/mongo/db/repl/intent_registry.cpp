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

#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/decorable.h"

#include <mutex>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {

namespace rss {
namespace consensus {
namespace {

auto registryDecoration = ServiceContext::declareDecoration<IntentRegistry>();

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
    auto state = repl::ReplicationCoordinator::get(opCtx->getServiceContext())->getMemberState();

    std::shared_lock lock(_stateMutex);

    // Downgrade Write intent to LocalWrite during initial sync.
    if (intent == Intent::Write && isReplSet &&
        (state == repl::MemberState::RS_STARTUP2 || state == repl::MemberState::RS_REMOVED)) {
        intent = Intent::LocalWrite;
    }

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
                ClientLock lock(client);
                serviceCtx->killOperation(lock, opCtx, ErrorCodes::InterruptedAtShutdown);
            }
            uassert(ErrorCodes::InterruptedAtShutdown,
                    "Cannot register intent due to Shutdown.",
                    validIntent);
        } else {
            uassert(ErrorCodes::InterruptedDueToReplStateChange,
                    "Cannot register intent due to ReplStateChange.",
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
    LOGV2_DEBUG(9945004,
                3,
                "Register Intent",
                "token"_attr = token.id(),
                "intent"_attr = intentToString(intent),
                "opCtx"_attr = opCtx->getOpID());
    {
        stdx::unique_lock<stdx::mutex> lockTokenMap(tokenMap.lock);
        tokenMap.map.insert({token.id(), opCtx});
    }

    return token;
}
void IntentRegistry::deregisterIntent(IntentRegistry::IntentToken token) {
    auto& tokenMap = _tokenMaps[(size_t)token.intent()];
    stdx::lock_guard<stdx::mutex> lock(tokenMap.lock);

    (void)tokenMap.map.erase(token.id());
    if (tokenMap.map.empty()) {
        tokenMap.cv.notify_all();
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

stdx::future<ReplicationStateTransitionGuard> IntentRegistry::killConflictingOperations(
    IntentRegistry::InterruptionType interrupt,
    OperationContext* opCtx,
    boost::optional<uint32_t> timeout_sec) {
    LOGV2(9945003, "Intent Registry killConflictingOperations", "interrupt"_attr = interrupt);
    _pendingStateChange.fetchAndAdd(1);
    auto timeOutSec = stdx::chrono::seconds(
        timeout_sec ? *timeout_sec : repl::fassertOnLockTimeoutForStepUpDown.load());

    _waitForDrain(Intent::BlockingWrite, stdx::chrono::milliseconds(0));
    {
        stdx::unique_lock lock(_stateMutex);
        if (_interruptionCtx) {
            LOGV2(9945001, "Existing kill ongoing. Blocking until it is finished.");
        }

        _activeInterruptionCV.wait(lock, [this] { return !_interruptionCtx; });
        _lastInterruption = interrupt;
        _interruptionCtx = opCtx;
    }

    return stdx::async(stdx::launch::async, [&, interrupt, timeOutSec] {
        const std::vector<Intent>* intents = nullptr;
        switch (interrupt) {
            case InterruptionType::Rollback: {
                static const std::vector<Intent> rollbackIntents = {Intent::Write, Intent::Read};
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
            Timer timer;
            auto timeout = stdx::chrono::duration_cast<stdx::chrono::milliseconds>(timeOutSec);
            for (auto intent : *intents) {
                _waitForDrain(intent, timeout);
                // Negative duration to cv::wait_for can cause undefined behavior
                // Since timeout == 0 is a special case to enable untimed wait we prevent a
                // non-zero timeout to ever drop to 0 by setting it to at least to 1ms
                if (timeout.count()) {
                    timeout -= std::min(
                        stdx::chrono::milliseconds(durationCount<Milliseconds>(timer.elapsed())),
                        timeout - 1ms);
                }
            }
        }

        updateAndLogStateTransitionMetrics(interrupt, _totalOpsKilled);
        _totalOpsKilled = 0;

        return ReplicationStateTransitionGuard([&]() {
            stdx::lock_guard lock(_stateMutex);
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

void IntentRegistry::enable() {
    stdx::lock_guard lock(_stateMutex);
    _enabled = true;
    _lastInterruption = InterruptionType::None;
    _interruptionCtx = nullptr;
    _activeInterruptionCV.notify_one();
}

void IntentRegistry::disable() {
    stdx::lock_guard lock(_stateMutex);
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
    stdx::lock_guard<stdx::mutex> lock(tokenMap.lock);
    for (auto& [token, toKill] : tokenMap.map) {
        auto serviceCtx = toKill->getServiceContext();
        auto client = toKill->getClient();
        if (interruption == InterruptionType::StepDown && !client->canKillOperationInStepdown()) {
            LOGV2(10336502,
                  "Skipping killing intent for stepdown due to unkillable client",
                  "name"_attr = client->desc(),
                  "registered_token"_attr = token);
            continue;
        }
        // Do not kill opCtx's that are inside an UninterruptibleLockGuard.
        if (toKill->uninterruptibleLocksRequested_DO_NOT_USE()) {  // NOLINT
            LOGV2(10336500,
                  "Skipping killing intent due to UninterruptibleLockGuard",
                  "name"_attr = client->desc(),
                  "registered_token"_attr = token);
            continue;
        }
        ClientLock lock(client);
        if (interruption == InterruptionType::Shutdown) {
            serviceCtx->killOperation(lock, toKill, ErrorCodes::InterruptedAtShutdown);
        } else {
            serviceCtx->killOperation(lock, toKill, ErrorCodes::InterruptedDueToReplStateChange);
        }
        _totalOpsKilled += 1;
        LOGV2(9795400,
              "Repl state change interrupted a thread.",
              "name"_attr = client->desc(),
              "registered token"_attr = token,
              "killcode"_attr = toKill->getKillStatus());
    }
}

void IntentRegistry::_waitForDrain(IntentRegistry::Intent intent,
                                   stdx::chrono::milliseconds timeout) {
    auto& tokenMap = _tokenMaps[(size_t)intent];
    stdx::unique_lock<stdx::mutex> lock(tokenMap.lock);
    if (timeout.count() &&
        !tokenMap.cv.wait_for(lock, timeout, [&tokenMap] { return tokenMap.map.empty(); })) {
        LOGV2(
            9795403, "There are still registered intents", "Intent"_attr = intentToString(intent));
        for (auto& [token, opCtx] : tokenMap.map) {
            LOGV2(9795402,
                  "Registered token:",
                  "token_id"_attr = token,
                  "client"_attr = opCtx->getClient()->desc());
        }
        LOGV2(9795404, "Timeout while waiting on intent queue to drain");
        fasserted(9795401);
    } else if (!timeout.count()) {
        tokenMap.cv.wait(lock, [&tokenMap] { return tokenMap.map.empty(); });
    }
}

size_t IntentRegistry::getTotalOpsKilled() const {
    return _totalOpsKilled;
}

std::vector<size_t> IntentRegistry::getTotalIntentsDeclared() const {
    auto getTotalIntents = [&](IntentRegistry::Intent intent) {
        auto& tokenMap = _tokenMaps[(size_t)intent];
        stdx::unique_lock<stdx::mutex> lock(tokenMap.lock);
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
