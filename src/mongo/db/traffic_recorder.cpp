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

#include "mongo/db/traffic_recorder.h"

#include "mongo/base/data_builder.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_type_terminated.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sorter/sorter_checksum_calculator.h"
#include "mongo/db/traffic_recorder.h"
#include "mongo/db/traffic_recorder_gen.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>  // IWYU pragma: keep
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/path.hpp>

namespace mongo {

namespace {

bool shouldAlwaysRecordTraffic = false;

MONGO_INITIALIZER(ShouldAlwaysRecordTraffic)(InitializerContext*) {
    if (!gAlwaysRecordTraffic.size()) {
        return;
    }

    if (gTrafficRecordingDirectory.empty()) {
        if (serverGlobalParams.logpath.empty()) {
            uasserted(ErrorCodes::BadValue,
                      "invalid to set AlwaysRecordTraffic without a logpath or "
                      "trafficRecordingDirectory");
        } else {
            gTrafficRecordingDirectory = serverGlobalParams.logpath;
        }
    }

    shouldAlwaysRecordTraffic = true;
}

}  // namespace

void appendPacketHeader(DataBuilder& db, const TrafficRecordingPacket& packet) {
    db.clear();
    Message toWrite = packet.message;

    uassertStatusOK(db.writeAndAdvance<LittleEndian<uint32_t>>(0));
    uassertStatusOK(db.writeAndAdvance<EventType>(packet.eventType));
    uassertStatusOK(db.writeAndAdvance<LittleEndian<uint64_t>>(packet.id));
    uassertStatusOK(db.writeAndAdvance<Terminated<'\0', StringData>>(StringData(packet.session)));
    uassertStatusOK(
        db.writeAndAdvance<LittleEndian<uint64_t>>(durationCount<Microseconds>(packet.offset)));
    uassertStatusOK(db.writeAndAdvance<LittleEndian<uint64_t>>(packet.order));

    auto fullSize = db.size() + packet.message.size();
    db.getCursor().write<LittleEndian<uint32_t>>(fullSize);
}

TrafficRecorder::Recording::Recording(const StartTrafficRecording& options, TickSource* tickSource)
    : _path(_getPath(std::string{options.getDestination()})),
      _maxLogSize(options.getMaxFileSize()) {

    MultiProducerSingleConsumerQueue<TrafficRecordingPacket, CostFunction>::Options queueOptions;
    queueOptions.maxQueueDepth = options.getMaxMemUsage();
    if (!shouldAlwaysRecordTraffic) {
        queueOptions.maxProducerQueueDepth = 0;
    }
    _pcqPipe =
        MultiProducerSingleConsumerQueue<TrafficRecordingPacket, CostFunction>::Pipe(queueOptions);

    _trafficStats.setRunning(true);
    _trafficStats.setBufferSize(options.getMaxMemUsage());
    _trafficStats.setRecordingDir(_path);
    _trafficStats.setMaxFileSize(_maxLogSize);

    startTime.store(tickSource->ticksTo<Microseconds>(tickSource->getTicks()));
}

void TrafficRecorder::Recording::run() {
    _thread = stdx::thread([consumer = std::move(_pcqPipe.consumer), this] {
        if (!boost::filesystem::is_directory(boost::filesystem::absolute(_path))) {
            boost::filesystem::create_directory(boost::filesystem::absolute(_path));
        }

        static const std::string checkSumFileName = "checksum.txt";
        boost::filesystem::path checksumFile(boost::filesystem::absolute(_path));
        checksumFile /= checkSumFileName;
        boost::filesystem::ofstream checksumOut(checksumFile,
                                                std::ios_base::app | std::ios_base::out);

        // The calculator calculates the checksum of each recording for integrity check.
        SorterChecksumCalculator checksumCalculator = {SorterChecksumVersion::v2};
        auto writeChecksum = [&checksumOut, &checksumCalculator](const std::string& recordingFile) {
            auto checkSumVal = checksumCalculator.hexdigest();
            checksumOut << recordingFile << "\t" << checkSumVal << std::endl;
            checksumCalculator.reset();
        };

        // This function guarantees to open a new recording file. Force the thread to sleep for
        // a very short period of time if a file with the same name exists and then create a new
        // file. This case is rare and only happens when the 'maxFileSize' is too small. The
        // same recording file could be opened twice within 1 millisecond.
        auto openNewRecordingFile = [this](boost::filesystem::path& recordingFile,
                                           boost::filesystem::ofstream& out) {
            recordingFile = boost::filesystem::absolute(_path);
            recordingFile /= std::to_string(Date_t::now().toMillisSinceEpoch());
            recordingFile += ".bin";
            while (boost::filesystem::exists(recordingFile)) {
                stdx::this_thread::sleep_for(stdx::chrono::milliseconds(5));
                recordingFile = boost::filesystem::absolute(_path);
                recordingFile /= std::to_string(Date_t::now().toMillisSinceEpoch());
                recordingFile += ".bin";
            }
            out.open(recordingFile,
                     std::ios_base::binary | std::ios_base::trunc | std::ios_base::out);
        };
        boost::filesystem::path recordingFile;
        boost::filesystem::ofstream out;
        try {
            DataBuilder db;
            openNewRecordingFile(recordingFile, out);

            while (true) {
                std::deque<TrafficRecordingPacket> storage;
                size_t bytes;

                std::tie(storage, bytes) = consumer.popManyUpTo(MaxMessageSizeBytes);

                // if this fired... somehow we got a message bigger than a message
                invariant(bytes);

                for (const auto& packet : storage) {
                    appendPacketHeader(db, packet);
                    Message toWrite = packet.message;

                    auto size = db.size() + toWrite.size();

                    bool maxSizeExceeded = false;

                    {
                        stdx::lock_guard<stdx::mutex> lk(_mutex);
                        _written += size;
                        maxSizeExceeded = _written >= _maxLogSize;
                    }

                    if (maxSizeExceeded) {
                        writeChecksum(recordingFile.string());
                        out.close();
                        // The current recording file hits the maximum file size, open a new
                        // recording file.
                        openNewRecordingFile(recordingFile, out);
                        // We assume that the size of one packet message is greater than the max
                        // file size. It's intentional to not assert if
                        // 'size' >= '_maxLogSize' for testing purposes.
                        {
                            stdx::lock_guard<stdx::mutex> lk(_mutex);
                            _written = size;
                        }
                    }

                    out.write(db.getCursor().data(), db.size());
                    checksumCalculator.addData(db.getCursor().data(), db.size());
                    out.write(toWrite.buf(), toWrite.size());
                    checksumCalculator.addData(toWrite.buf(), toWrite.size());
                }
            }
        } catch (const ExceptionFor<ErrorCodes::ProducerConsumerQueueConsumed>&) {
            // Close naturally
            writeChecksum(recordingFile.string());
        } catch (...) {
            writeChecksum(recordingFile.string());
            auto status = exceptionToStatus();

            stdx::lock_guard<stdx::mutex> lk(_mutex);
            _result = status;
        }
    });
}

bool TrafficRecorder::Recording::pushRecord(const uint64_t id,
                                            const std::string session,
                                            Microseconds offset,
                                            const uint64_t& order,
                                            const Message& message,
                                            EventType eventType) {
    try {
        _pcqPipe.producer.push({eventType, id, session, offset, order, message});
        return true;
    } catch (const ExceptionFor<ErrorCodes::ProducerConsumerQueueProducerQueueDepthExceeded>&) {
        invariant(!shouldAlwaysRecordTraffic);

        // If we couldn't push our packet begin the process of failing the recording
        _pcqPipe.producer.close();

        stdx::lock_guard<stdx::mutex> lk(_mutex);

        // If the result was otherwise okay, mark it as failed due to the queue blocking.  If
        // it failed for another reason, don't overwrite that.
        if (_result.isOK()) {
            _result = Status(ErrorCodes::Error(51061), "queue was blocked in traffic recorder");
        }
    } catch (const ExceptionFor<ErrorCodes::ProducerConsumerQueueEndClosed>&) {
    }

    return false;
}

Status TrafficRecorder::Recording::shutdown() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    if (!_inShutdown) {
        _inShutdown = true;
        lk.unlock();

        _pcqPipe.producer.close();
        _thread.join();

        lk.lock();
    }

    return _result;
}

BSONObj TrafficRecorder::Recording::getStats() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _trafficStats.setBufferedBytes(_pcqPipe.controller.getStats().queueDepth);
    _trafficStats.setCurrentFileSize(_written);
    return _trafficStats.toBSON();
}

std::string TrafficRecorder::Recording::_getPath(const std::string& filename) {
    uassert(
        ErrorCodes::BadValue, "Traffic recording filename must not be empty", !filename.empty());

    if (gTrafficRecordingDirectory.back() == '/') {
        gTrafficRecordingDirectory.pop_back();
    }
    auto parentPath = boost::filesystem::path(gTrafficRecordingDirectory);
    auto path = parentPath / filename;

    uassert(ErrorCodes::BadValue,
            "Traffic recording filename must be a simple filename",
            path.parent_path() == parentPath);

    return path.string();
}


namespace {
static const auto getTrafficRecorder = ServiceContext::declareDecoration<TrafficRecorder>();
}  // namespace

TrafficRecorder& TrafficRecorder::get(ServiceContext* svc) {
    return getTrafficRecorder(svc);
}

TrafficRecorder::TrafficRecorder() : _shouldRecord(shouldAlwaysRecordTraffic) {}

TrafficRecorder::~TrafficRecorder() {
    if (shouldAlwaysRecordTraffic) {
        (**_recording)->shutdown().ignore();
    }
}

std::shared_ptr<TrafficRecorder::Recording> TrafficRecorder::_makeRecording(
    const StartTrafficRecording& options, TickSource* tickSource) const {
    return std::make_shared<Recording>(options, tickSource);
}

void TrafficRecorder::start(const StartTrafficRecording& options, ServiceContext* svcCtx) {
    invariant(!shouldAlwaysRecordTraffic);

    uassert(ErrorCodes::BadValue,
            "Traffic recording directory not set",
            !gTrafficRecordingDirectory.empty());

    {
        auto rec = _recording.synchronize();
        uassert(ErrorCodes::BadValue, "Traffic recording already active", !*rec);
        *rec = _makeRecording(options, svcCtx->getTickSource());

        (*rec)->run();
    }
    _shouldRecord.store(true);
    {
        // Record SessionStart events if exists any active session.
        stdx::lock_guard<stdx::recursive_mutex> sessionLk(_openSessionsLk);
        for (const auto& [id, session] : _openSessions) {
            observe(id, session, Message(), svcCtx, EventType::kSessionStart);
        }
    }
}

void TrafficRecorder::updateOpenSessions(uint64_t id,
                                         const std::string& session,
                                         EventType eventType) {
    if (eventType == EventType::kSessionEnd) {
        stdx::lock_guard<stdx::recursive_mutex> lk(_openSessionsLk);
        auto sessionItr = _openSessions.find(id);
        if (sessionItr != _openSessions.end()) {
            _openSessions.erase(sessionItr);
        }
    }

    if (eventType == EventType::kSessionStart) {
        stdx::lock_guard<stdx::recursive_mutex> lk(_openSessionsLk);
        _openSessions.emplace(id, session);
    }
}

void TrafficRecorder::stop(ServiceContext* svcCtx) {
    invariant(!shouldAlwaysRecordTraffic);
    // Record SessionEnd events if exists any active session.
    {
        stdx::lock_guard<stdx::recursive_mutex> lk(_openSessionsLk);
        // A copy of open sessions to remove from '_openSessionsLk' while observing a SessionEnd
        // event for each open session. 'observe()' will modify '_openSessions'.
        stdx::unordered_map<uint64_t, std::string> sessions(_openSessions);

        for (const auto& [id, session] : sessions) {
            observe(id, session, Message(), svcCtx, EventType::kSessionEnd);
        }
    }

    _shouldRecord.store(false);

    auto recording = [&] {
        auto rec = _recording.synchronize();
        uassert(ErrorCodes::BadValue, "Traffic recording not active", *rec);

        return std::move(*rec);
    }();

    uassertStatusOK(recording->shutdown());
}

void TrafficRecorder::observe(const std::shared_ptr<transport::Session>& ts,
                              const Message& message,
                              ServiceContext* svcCtx,
                              EventType eventType) {
    observe(ts->id(), ts->toBSON().toString(), message, svcCtx, eventType);
}

void TrafficRecorder::observe(uint64_t id,
                              const std::string& session,
                              const Message& message,
                              ServiceContext* svcCtx,
                              EventType eventType) {
    // Keep track of active sessions not recording anything. Session start/end events will be
    // recorded on the start/stop of the traffic recording.
    if (eventType == EventType::kSessionEnd || eventType == EventType::kSessionStart) {
        updateOpenSessions(id, session, eventType);
    }
    auto* tickSource = svcCtx->getTickSource();
    if (shouldAlwaysRecordTraffic) {
        auto rec = _recording.synchronize();

        if (!*rec) {
            StartTrafficRecording options;
            options.setDestination(gAlwaysRecordTraffic);
            options.setMaxFileSize({double(std::numeric_limits<int64_t>::max())});

            *rec = _makeRecording(options, tickSource);
            (*rec)->run();
        }

        invariant((*rec)->pushRecord(id,
                                     session,
                                     tickSource->ticksTo<Microseconds>(tickSource->getTicks()) -
                                         (*rec)->startTime.load(),
                                     (*rec)->order.addAndFetch(1),
                                     message,
                                     eventType));
        return;
    }
    if (!_shouldRecord.load()) {
        return;
    }

    auto recording = _getCurrentRecording();

    // If we don't have an active recording, bail
    if (!recording) {
        return;
    }

    // Try to record the message
    if (recording->pushRecord(id,
                              session,
                              tickSource->ticksTo<Microseconds>(tickSource->getTicks()) -
                                  recording->startTime.load(),
                              recording->order.addAndFetch(1),
                              message,
                              eventType)) {
        return;
    }

    // If the recording isn't the one we have in hand bail (its been ended, or a new one has
    // been created
    if (**_recording != recording) {
        return;
    }

    // We couldn't queue and it's still our recording.  No one else should try to queue
    _shouldRecord.store(false);
}

std::shared_ptr<TrafficRecorder::Recording> TrafficRecorder::_getCurrentRecording() const {
    return *_recording;
}

class TrafficRecorder::TrafficRecorderSSS : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        auto& recorder = TrafficRecorder::get(opCtx->getServiceContext());

        if (!recorder._shouldRecord.load()) {
            return BSON("running" << false);
        }

        auto recording = recorder._getCurrentRecording();

        if (!recording) {
            return BSON("running" << false);
        }

        return recording->getStats();
    }
};
auto& trafficRecorderStats =
    *ServerStatusSectionBuilder<TrafficRecorder::TrafficRecorderSSS>("trafficRecording");

std::shared_ptr<TrafficRecorderForTest::RecordingForTest>
TrafficRecorderForTest::getCurrentRecording() const {
    return std::dynamic_pointer_cast<TrafficRecorderForTest::RecordingForTest>(
        _getCurrentRecording());
}


std::shared_ptr<TrafficRecorder::Recording> TrafficRecorderForTest::_makeRecording(
    const StartTrafficRecording& options, TickSource* tickSource) const {
    return std::make_shared<TrafficRecorderForTest::RecordingForTest>(options, tickSource);
}

TrafficRecorderForTest::RecordingForTest::RecordingForTest(const StartTrafficRecording& options,
                                                           TickSource* tickSource)
    : TrafficRecorder::Recording(options, tickSource) {}

MultiProducerSingleConsumerQueue<TrafficRecordingPacket,
                                 TrafficRecorder::Recording::CostFunction>::Pipe&
TrafficRecorderForTest::RecordingForTest::getPcqPipe() {
    return _pcqPipe;
}

void TrafficRecorderForTest::RecordingForTest::run() {
    // No-op for tests
}

Status TrafficRecorderForTest::RecordingForTest::shutdown() {
    // No-op for tests
    return Status::OK();
}

}  // namespace mongo
