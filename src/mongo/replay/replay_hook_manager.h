// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/traffic_reader.h"
#include "mongo/replay/replay_command.h"
#include "mongo/util/modules.h"

#include <concepts>
#include <vector>

namespace mongo {


/**
 * Interface to be implemented by types which wish to inspect, and possibly modify, events
 * immediately prior to replay.
 *
 * Observers registered per-session will be invoked by a single thread.
 *
 * Observers registered globally may be invoked by multiple threads concurrently (on different
 * packets); it is the implementer's responsibility to ensure this is safe.
 */
class ReplayObserver {
public:
    virtual void onRequest(ReplayCommand& command) {}
    virtual void onResponse(ReplayCommand& command) {}
    virtual void onSessionStart(ReplayCommand& command) {}
    virtual void onSessionEnd(ReplayCommand& command) {}

    virtual void onLiveResponse(const ReplayCommand& recordedResponse,
                                const BSONObj& liveResponse) {}

    virtual ~ReplayObserver() = default;
};

class PerSessionObserverState;

class ReplayObserverManager {
public:
    static ReplayObserverManager& get();

    PerSessionObserverState makeSessionObservers();

    template <std::derived_from<ReplayObserver> ConcreteHook>
    void registerPerSessionObserver() {
        registerPerSessionObserver([] { return std::make_shared<ConcreteHook>(); });
    }
    void registerPerSessionObserver(std::function<std::shared_ptr<ReplayObserver>()> hookFactory);
    void registerObserver(std::shared_ptr<ReplayObserver> hook);

    /**
     * Invoke all registered hooks, in order of registration.
     *
     * Not safe to call concurrently with registerObserver; all hooks should
     * be registered before processing any packets.
     */
    void observe(ReplayCommand& command);

    void observeLiveResponse(const ReplayCommand& recordedResponse, const BSONObj& liveResponse);

    /**
     * Test-only method to clear registered observers.
     *
     * Not safe to use outside of unit tests which can guarantee no concurrent access
     * from a recording.
     */
    void reset();

private:
    // Observers will be invoked for all events across all sessions, potentially
    // concurrently.
    std::vector<std::shared_ptr<ReplayObserver>> _observers;
    // Each session will construct an instance of every currently registered observer here.
    std::vector<std::function<std::shared_ptr<ReplayObserver>()>> _sessionObserverFactories;
};

class PerSessionObserverState {
public:
    PerSessionObserverState(std::vector<std::shared_ptr<ReplayObserver>> observers)
        : _observers(observers) {}
    void observe(ReplayCommand& command);
    void observeLiveResponse(const ReplayCommand& recordedResponse, const BSONObj& liveResponse);

private:
    // These observers will be invoked from a single thread, for a single session.
    std::vector<std::shared_ptr<ReplayObserver>> _observers;
};
}  // namespace mongo
