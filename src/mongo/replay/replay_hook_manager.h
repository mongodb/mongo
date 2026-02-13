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
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/traffic_reader.h"
#include "mongo/replay/replay_command.h"

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
