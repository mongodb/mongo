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

#include "mongo/replay/replay_hook_manager.h"

#include "mongo/db/traffic_reader.h"
#include "mongo/db/traffic_recorder_event.h"

#include <vector>

namespace mongo {

ReplayObserverManager& ReplayObserverManager::get() {
    static ReplayObserverManager mgr;
    return mgr;
}

PerSessionObserverState ReplayObserverManager::makeSessionObservers() {
    std::vector<std::shared_ptr<ReplayObserver>> hooks;
    hooks.reserve(_sessionObserverFactories.size());
    for (const auto& factory : _sessionObserverFactories) {
        hooks.push_back(factory());
    }
    return PerSessionObserverState(std::move(hooks));
}

void ReplayObserverManager::registerPerSessionObserver(
    std::function<std::shared_ptr<ReplayObserver>()> hookFactory) {
    _sessionObserverFactories.push_back(std::move(hookFactory));
}

void ReplayObserverManager::registerObserver(std::shared_ptr<ReplayObserver> hook) {
    _observers.push_back(std::move(hook));
}

auto getHookMethod(EventType eventType) {
    switch (eventType) {
        case EventType::kRequest:
            return &ReplayObserver::onRequest;
        case EventType::kResponse:
            return &ReplayObserver::onResponse;
        case EventType::kSessionStart:
            return &ReplayObserver::onSessionStart;
        case EventType::kSessionEnd:
            return &ReplayObserver::onSessionEnd;
        default:
            throw std::logic_error("Invalid event type");
    }
}

void ReplayObserverManager::observe(ReplayCommand& command) {
    auto method = getHookMethod(command.getEventType());
    for (const auto& hook : _observers) {
        (hook.get()->*method)(command);
    }
}

void ReplayObserverManager::observeLiveResponse(const ReplayCommand& recordedResponse,
                                                const BSONObj& liveResponse) {
    for (const auto& hook : _observers) {
        hook->onLiveResponse(recordedResponse, liveResponse);
    }
}

void PerSessionObserverState::observe(ReplayCommand& command) {
    // Invoke per-session observers first.
    auto method = getHookMethod(command.getEventType());
    for (const auto& hook : _observers) {
        (hook.get()->*method)(command);
    }

    // Dispatch to replay-wide observers.
    ReplayObserverManager::get().observe(command);
}

void PerSessionObserverState::observeLiveResponse(const ReplayCommand& recordedResponse,
                                                  const BSONObj& liveResponse) {
    // Invoke per-session hooks first.
    for (const auto& hook : _observers) {
        hook->onLiveResponse(recordedResponse, liveResponse);
    }

    // Dispatch to replay-wide observers.
    ReplayObserverManager::get().observeLiveResponse(recordedResponse, liveResponse);
}
}  // namespace mongo
