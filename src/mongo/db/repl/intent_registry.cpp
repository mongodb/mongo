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

#include <mutex>

#include "mongo/db/repl/intent_registry.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/util/decorable.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {

namespace rss {
namespace consensus {
namespace {

auto registryDecoration = ServiceContext::declareDecoration<IntentRegistry>();

}  // namespace

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

    stdx::lock_guard<stdx::mutex> lock(_stateMutex);

    uassert(ErrorCodes::InterruptedDueToReplStateChange,
            "Cannot register intent due to ReplStateChange.",
            _validIntent((intent)));

    bool isReplSet =
        repl::ReplicationCoordinator::get(opCtx->getServiceContext())->getSettings().isReplSet();

    if (isReplSet && intent == Intent::Write) {
        bool isWritablePrimary =
            repl::ReplicationCoordinator::get(opCtx->getServiceContext())
                ->canAcceptWritesFor(opCtx, NamespaceString(DatabaseName::kAdmin));
        uassert(ErrorCodes::NotWritablePrimary,
                "Cannot register write intent if we are not primary.",
                isWritablePrimary);
    }

    auto& tokenMap = _tokenMaps[(size_t)intent];
    IntentToken token(intent);
    LOGV2(9945004, "Register Intent", "token"_attr = token.id(), "intent"_attr = intent);
    {
        stdx::unique_lock<stdx::mutex> lockTokenMap(tokenMap.lock);
        tokenMap.map.insert({token.id(), opCtx});
    }
    {
        stdx::unique_lock<stdx::mutex> lockOpCtxIntentMap(_opCtxIntentMap.lock);
        auto ins = _opCtxIntentMap.map.insert({opCtx, token.intent()});
        uassert(1026190, "Operation context already has a registered intent.", ins.second);
    }
    return token;
}
void IntentRegistry::deregisterIntent(IntentRegistry::IntentToken token) {
    auto& tokenMap = _tokenMaps[(size_t)token.intent()];
    stdx::lock_guard<stdx::mutex> lock(tokenMap.lock);
    {
        stdx::unique_lock<stdx::mutex> lockOpCtxIntentMap(_opCtxIntentMap.lock);
        // Find IntentToken:opCtx pair in tokenMap.
        auto tokenMapIter = tokenMap.map.find(token.id());
        if (tokenMapIter != tokenMap.map.end()) {
            auto opCtx = tokenMapIter->second;
            _opCtxIntentMap.map.erase(opCtx);
        }
    }

    (void)tokenMap.map.erase(token.id());
    if (tokenMap.map.empty()) {
        tokenMap.cv.notify_all();
    }
}

stdx::future<ReplicationStateTransitionGuard> IntentRegistry::killConflictingOperations(
    IntentRegistry::InterruptionType interrupt) {
    LOGV2(9945003, "Intent Registry killConflictingOperations", "interrupt"_attr = interrupt);
    {
        stdx::unique_lock<stdx::mutex> lock(_stateMutex);
        if (_activeInterruption) {
            LOGV2(9945001, "Existing kill ongoing. Blocking until it is finished.");
        }

        if (!activeInterruptionCV.wait_for(
                lock, _drainTimeoutSec, [this] { return !_activeInterruption; })) {
            LOGV2(9945005,
                  "Timeout while wating on a previous interrupt to drain.",
                  "last"_attr = _lastInterruption,
                  "new"_attr = interrupt);
            fasserted(9945002);
        }
        _activeInterruption = true;
        _lastInterruption = interrupt;
    }
    return stdx::async(stdx::launch::async, [&, interrupt] {
        const std::vector<Intent>* intents = nullptr;
        switch (interrupt) {
            case InterruptionType::Rollback:
            case InterruptionType::Shutdown: {
                static const std::vector<Intent> rollbackShutdownIntents = {
                    Intent::Write, Intent::Read, Intent::LocalWrite};
                intents = &rollbackShutdownIntents;
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
                _killOperationsByIntent(intent);
            }
            Timer timer;
            auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(_drainTimeoutSec);
            for (auto intent : *intents) {
                _waitForDrain(intent, timeout);
                // Negative duration to cv::wait_for can cause undefined behavior
                timeout -= std::min(
                    std::chrono::milliseconds(durationCount<Milliseconds>(timer.elapsed())),
                    timeout);
            }
        }
        return ReplicationStateTransitionGuard([&]() {
            stdx::lock_guard<stdx::mutex> lock(_stateMutex);
            _activeInterruption = false;
            _lastInterruption = InterruptionType::None;
        });
    });
}

void IntentRegistry::enable() {
    stdx::lock_guard<stdx::mutex> lock(_stateMutex);
    _enabled = true;
    _activeInterruption = false;
    _lastInterruption = InterruptionType::None;
}

void IntentRegistry::disable() {
    stdx::lock_guard<stdx::mutex> lock(_stateMutex);
    _enabled = false;
}

void IntentRegistry::setDrainTimeout(uint32_t sec) {
    _drainTimeoutSec = std::chrono::seconds(sec);
}

boost::optional<IntentRegistry::Intent> IntentRegistry::getHeldIntent(
    OperationContext* opCtx) const {
    stdx::unique_lock<stdx::mutex> lockOpCtxIntentMap(_opCtxIntentMap.lock);
    auto iter = _opCtxIntentMap.map.find(opCtx);
    if (iter != _opCtxIntentMap.map.end()) {
        return iter->second;
    } else {
        return boost::none;
    }
}

bool IntentRegistry::isIntentHeld(OperationContext* opCtx) const {
    return (getHeldIntent(opCtx) != boost::none);
}

bool IntentRegistry::_validIntent(IntentRegistry::Intent intent) const {
    if (!_enabled) {
        return false;
    }
    switch (_lastInterruption) {
        case InterruptionType::Rollback:
        case InterruptionType::Shutdown:
            return false;
        case InterruptionType::StepDown:
        case InterruptionType::StepUp:
            return intent != Intent::Write;
        default:
            return true;
    }
}

void IntentRegistry::_killOperationsByIntent(IntentRegistry::Intent intent) {
    auto& tokenMap = _tokenMaps[(size_t)intent];
    stdx::lock_guard<stdx::mutex> lock(tokenMap.lock);
    for (auto& [token, toKill] : tokenMap.map) {
        auto serviceCtx = toKill->getServiceContext();
        auto client = toKill->getClient();
        ClientLock lock(client);
        serviceCtx->killOperation(lock, toKill, ErrorCodes::InterruptedDueToReplStateChange);
        LOGV2(9795400,
              "Repl state change interrupted a thread.",
              "name"_attr = client->desc(),
              "registered token"_attr = token,
              "killcode"_attr = toKill->getKillStatus());
    }
}

void IntentRegistry::_waitForDrain(IntentRegistry::Intent intent,
                                   std::chrono::milliseconds timeout) {
    auto& tokenMap = _tokenMaps[(size_t)intent];
    stdx::unique_lock<stdx::mutex> lock(tokenMap.lock);
    if (!tokenMap.cv.wait_for(lock, timeout, [&tokenMap] { return tokenMap.map.empty(); })) {
        LOGV2(
            9795403, "There are still registered intents", "Intent"_attr = _intentToString(intent));
        for (auto& [token, opCtx] : tokenMap.map) {
            LOGV2(9795402, "Registered token:", "token_id"_attr = token);
        }
        LOGV2(9795404, "Timeout while wating on intent queue to drain");
        fasserted(9795401);
    }
}

std::string IntentRegistry::_intentToString(IntentRegistry::Intent intent) {
    switch (intent) {
        case Intent::LocalWrite:
            return "LOCAL_WRITE";
        case Intent::Read:
            return "READ";
        case Intent::Write:
            return "WRITE";
        default:
            return "UNKNOWN";
    }
}

}  // namespace consensus
}  // namespace rss
}  // namespace mongo
