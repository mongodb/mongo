/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/db/service_context.h"
#include "mongo/db/traffic_recorder_gen.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/message.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/session.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <queue>
#include <string>

#include <boost/optional.hpp>

namespace mongo {

enum class EventType : uint8_t {
    kRegular = 0,       // A regular event.
    kSessionStart = 1,  // A non-message event indicating the start of a session.
    kSessionEnd = 2,    // A non-message event indicating the end of a session.

    kMax,  // Not a valid event type, used to check values are in-range.
};
struct TrafficRecordingPacket {
    EventType eventType;
    const uint64_t id;
    const std::string session;
    const Microseconds offset;  // Offset from the start of the recording in microseconds.
    const uint64_t order;
    const Message message;
};

class DataBuilder;
void appendPacketHeader(DataBuilder& builder, const TrafficRecordingPacket& packet);

/**
 * A service context level global which captures packet capture through the transport layer if it is
 * enabled.  The service is intended to be turned on and off via startTrafficRecording and
 * stopTrafficRecording.
 *
 * The recording can have one recording running at a time and the intention is that observe() blocks
 * callers for the least amount of time possible.
 */
class TrafficRecorder {
public:
    // The Recorder may record some special events that are required by the replay client.

    static TrafficRecorder& get(ServiceContext* svc);

    TrafficRecorder();
    virtual ~TrafficRecorder();

    // Start and stop block until the associate operation has succeeded or failed
    //
    // On failure these methods throw
    void start(const StartTrafficRecording& options, ServiceContext* svcCtx);
    void stop(ServiceContext* svcCtx);

    void observe(uint64_t id,
                 const std::string& session,
                 const Message& message,
                 ServiceContext* svcCtx,
                 EventType eventType = EventType::kRegular);

    // This is the main interface to record a message. It also maintains open sessions in order to
    // record 'kSessionStart' and 'kSessionEnd' events.
    // TODO SERVER-106769: change usage of TickSource/std::chrono::steady_clock to solution proposed
    // in SERVER-106769
    void observe(const std::shared_ptr<transport::Session>& ts,
                 const Message& message,
                 ServiceContext* svcCtx,
                 EventType eventType = EventType::kRegular);

    class TrafficRecorderSSS;

protected:
    /**
     * The Recording class represents a single recording that the recorder is exposing.  It's made
     * up of a background thread which flushes records to disk, and helper methods to push to that
     * thread, expose stats, and stop the recording.
     */
    class Recording {
    public:
        Recording(const StartTrafficRecording& options, TickSource* tickSource);
        virtual ~Recording() = default;

        virtual void run();
        virtual Status shutdown();

        /**
         * pushRecord returns false if the queue was full.  This is ultimately fatal to the
         * recording
         */
        bool pushRecord(uint64_t id,
                        std::string session,
                        Microseconds offset,
                        const uint64_t& order,
                        const Message& message,
                        EventType eventType = EventType::kRegular);

        BSONObj getStats();

        AtomicWord<uint64_t> order{0};
        AtomicWord<Microseconds> startTime{
            Microseconds::zero()};  // Start time of the recording in microseconds since the epoch.
    protected:
        struct CostFunction {
            size_t operator()(const TrafficRecordingPacket& packet) const {
                return sizeof(TrafficRecordingPacket) + packet.message.size();
            }
        };

        MultiProducerSingleConsumerQueue<TrafficRecordingPacket, CostFunction>::Pipe _pcqPipe;


    private:
        static std::string _getPath(const std::string& filename);

        const std::string _path;
        const int64_t _maxLogSize;

        stdx::thread _thread;

        stdx::mutex _mutex;
        bool _inShutdown = false;
        TrafficRecorderStats _trafficStats;
        int64_t _written = 0;
        Status _result = Status::OK();
    };

    // Helper method to be overridden in tests
    virtual std::shared_ptr<Recording> _makeRecording(const StartTrafficRecording& options,
                                                      TickSource* tickSource) const;

    void updateOpenSessions(uint64_t id, const std::string& session, EventType eventType);

    std::shared_ptr<Recording> _getCurrentRecording() const;

    AtomicWord<bool> _shouldRecord;

    mutable stdx::recursive_mutex _openSessionsLk;
    stdx::unordered_map<uint64_t, std::string> _openSessions;
    mongo::synchronized_value<std::shared_ptr<Recording>> _recording;
};

class TrafficRecorderForTest : public TrafficRecorder {
public:
    TrafficRecorderForTest() = default;
    ~TrafficRecorderForTest() override = default;


    class RecordingForTest : public TrafficRecorder::Recording {
    public:
        RecordingForTest(const StartTrafficRecording& options, TickSource* tickSource);

        // Accessor for testing purposes
        MultiProducerSingleConsumerQueue<TrafficRecordingPacket,
                                         TrafficRecorder::Recording::CostFunction>::Pipe&
        getPcqPipe();

        void run() override;

        Status shutdown() override;
    };

    std::shared_ptr<RecordingForTest> getCurrentRecording() const;

    std::shared_ptr<Recording> _makeRecording(const StartTrafficRecording& options,
                                              TickSource* tickSource) const override;
};
}  // namespace mongo
