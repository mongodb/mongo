// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/replay/replay_hook_manager.h"

#include "mongo/db/traffic_reader.h"
#include "mongo/db/traffic_recorder_event.h"

#include <vector>

namespace mongo {

ReplayObserverManager& ReplayObserverManager::get() {
    static ReplayObserverManager mgr;
    return mgr;
}

void ReplayObserverManager::reset() {
    _observers.clear();
    _sessionObserverFactories.clear();
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
