// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
/**
 * SessionSimulator handles events from a recording, and at the appropriate time point
 * dispatches requests to the contained ReplayCommandExecutor.
 */

#include "mongo/db/query/util/stop_token.h"
#include "mongo/replay/performance_reporter.h"
#include "mongo/replay/replay_command.h"
#include "mongo/replay/replay_command_executor.h"
#include "mongo/replay/traffic_recording_iterator.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <chrono>
#include <memory>
namespace mongo {

/**
 * SessionSimulator is a wrapper around ReplayCommandExecutor. The main difference between the two
 * is that SessionSimulator adds a simulation layer on top of ReplayCommandExecutor. This simulation
 * layers handles request timestamps or timestamps in general for initializing the simulation
 * itself. For example we may need to replay a very old session recorded for days and which not very
 * active. In this case an event can be recorded after hours of the previous one, but this won't
 * show up in the recording. So in this case we will get 2 events one after the other, but in
 * reality, to make the simulation as close as possible to the real execution, we will have to wait
 * a couple of hours before to run the command. So for this reason the current execution thread must
 * be put in sleep and resume later. ReplayCommandExecutor is a raw mongodb command executor, it
 * justs receives a bson, intrepets it in the correct way and sends it to the destination server,
 * where it is connected to. However, during a session simulation, we need to track when we started
 * and whether or not we want to simulate the execution of the current request right now.
 * SessionSimulator has a ReplayCommandExecutor, it does not inherit from it. We want to keep the
 * design as simple as possible. Performance is not very important in this case, however
 * SessionSimulator is not supposed to be thrown away and reconstructed constantly.
 */

using PacketSource = RecordingSetIterator;

class SessionSimulator : public std::enable_shared_from_this<SessionSimulator> {
public:
    SessionSimulator(PacketSource source,
                     uint64_t sessionID,
                     std::chrono::steady_clock::time_point globalStartTime,
                     std::string uri,
                     std::unique_ptr<ReplayCommandExecutor>,
                     std::unique_ptr<PerformanceReporter>);
    virtual ~SessionSimulator() = default;

    void run(mongo::stop_token stopToken = {});

protected:
    void start();
    void stop();
    BSONObj runCommand(const ReplayCommand&) const;

    virtual std::chrono::steady_clock::time_point now() const;
    virtual void sleepFor(std::chrono::steady_clock::duration duration) const;
    void waitIfNeeded(Microseconds) const;

    bool _running = false;
    // Timepoint used for computing when events should occur.
    // Derived from steady_clock not system_clock as the replay should
    // not be affected by clock manipulation (e.g., by NTP).
    std::chrono::steady_clock::time_point _replayStartTime;

    std::string _uri;
    PacketSource _source;
    uint64_t _sessionID;
    std::unique_ptr<ReplayCommandExecutor> _commandExecutor;
    std::unique_ptr<PerformanceReporter> _perfReporter;
};

}  // namespace mongo
