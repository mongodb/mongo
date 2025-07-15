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
/**
 * SessionSimulator handles events from a recording, and at the appropriate time point
 * dispatches requests to the contained ReplayCommandExecutor.
 */

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/replay/replay_command_executor.h"
#include "mongo/replay/session_scheduler.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

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
class SessionScheduler;
class ReplayCommandExecutor;
class SessionSimulator {
public:
    explicit SessionSimulator();
    virtual ~SessionSimulator();

    void start(StringData uri,
               std::chrono::steady_clock::time_point replayStartTime,
               const Date_t& recordStartTime,
               const Date_t& eventTimestamp);
    void stop(const Date_t& sessionEnd);
    void run(const ReplayCommand&, const Date_t& commandTimeStamp) const;

protected:
    /**
     * Halt all work, and join any spawned threads.
     *
     * Optional, only required if simulator must be halted before destruction.
     * (e.g., subclass needs to halt threads before destruction).
     */
    void shutdown();

private:
    virtual std::chrono::steady_clock::time_point now() const;
    virtual void sleepFor(std::chrono::steady_clock::duration duration) const;
    void waitIfNeeded(Date_t) const;
    void onRecordingStarted(Date_t);

    bool _running = false;
    // Timepoint used for computing when events should occur.
    // Derived from steady_clock not system_clock as the replay should
    // not be affected by clock manipulation (e.g., by NTP).
    std::chrono::steady_clock::time_point _replayStartTime;
    // TODO SERVER-106897: will change the recording format to use
    // "offset from start" for each event, rather than a wall clock
    // time point, making the wall clock recording start time
    // unnecessary.
    Date_t _recordStartTime;
    std::unique_ptr<ReplayCommandExecutor> _commandExecutor;
    std::unique_ptr<SessionScheduler> _sessionScheduler;
};

}  // namespace mongo
