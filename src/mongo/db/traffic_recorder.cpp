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
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sorter/sorter_checksum_calculator.h"
#include "mongo/db/traffic_recorder.h"
#include "mongo/db/traffic_recorder_gen.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/time_support.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>  // IWYU pragma: keep
#include <limits>
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

/**
 * The Recording class represents a single recording that the recorder is exposing.  It's made up of
 * a background thread which flushes records to disk, and helper methods to push to that thread,
 * expose stats, and stop the recording.
 */
class TrafficRecorder::Recording {
public:
    Recording(const StartTrafficRecording& options)
        : _path(_getPath(std::string{options.getDestination()})),
          _maxLogSize(options.getMaxFileSize()) {

        MultiProducerSingleConsumerQueue<TrafficRecordingPacket, CostFunction>::Options
            queueOptions;
        queueOptions.maxQueueDepth = options.getMaxMemUsage();
        if (!shouldAlwaysRecordTraffic) {
            queueOptions.maxProducerQueueDepth = 0;
        }
        _pcqPipe = MultiProducerSingleConsumerQueue<TrafficRecordingPacket, CostFunction>::Pipe(
            queueOptions);

        _trafficStats.setRunning(true);
        _trafficStats.setBufferSize(options.getMaxMemUsage());
        _trafficStats.setRecordingDir(_path);
        _trafficStats.setMaxFileSize(_maxLogSize);
    }

    void run() {
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
            auto checksumCalculator =
                std::make_unique<SorterChecksumCalculator>(SorterChecksumVersion::v2);
            auto writeChecksum = [&checksumOut,
                                  &checksumCalculator](const std::string& recordingFile) {
                size_t checkSumVal = checksumCalculator->checksum();
                checksumOut << recordingFile << "\t" << checkSumVal << "\n";
                checksumCalculator =
                    std::make_unique<SorterChecksumCalculator>(SorterChecksumVersion::v2);
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
                        db.clear();
                        Message toWrite = packet.message;

                        uassertStatusOK(db.writeAndAdvance<LittleEndian<uint32_t>>(0));
                        uassertStatusOK(db.writeAndAdvance<LittleEndian<uint64_t>>(packet.id));
                        uassertStatusOK(db.writeAndAdvance<Terminated<'\0', StringData>>(
                            StringData(packet.session)));
                        uassertStatusOK(db.writeAndAdvance<LittleEndian<uint64_t>>(
                            packet.now.toMillisSinceEpoch()));
                        uassertStatusOK(db.writeAndAdvance<LittleEndian<uint64_t>>(packet.order));

                        auto size = db.size() + toWrite.size();
                        db.getCursor().write<LittleEndian<uint32_t>>(size);

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
                        checksumCalculator->addData(db.getCursor().data(), db.size());
                        out.write(toWrite.buf(), toWrite.size());
                        checksumCalculator->addData(toWrite.buf(), toWrite.size());
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

    /**
     * pushRecord returns false if the queue was full.  This is ultimately fatal to the recording
     */
    bool pushRecord(const std::shared_ptr<transport::Session>& ts,
                    Date_t now,
                    const uint64_t order,
                    const Message& message) {
        try {
            _pcqPipe.producer.push({ts->id(), ts->toBSON().toString(), now, order, message});
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

    Status shutdown() {
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

    BSONObj getStats() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _trafficStats.setBufferedBytes(_pcqPipe.controller.getStats().queueDepth);
        _trafficStats.setCurrentFileSize(_written);
        return _trafficStats.toBSON();
    }

    AtomicWord<uint64_t> order{0};

private:
    struct TrafficRecordingPacket {
        const uint64_t id;
        const std::string session;
        const Date_t now;
        const uint64_t order;
        const Message message;
    };

    struct CostFunction {
        size_t operator()(const TrafficRecordingPacket& packet) const {
            return packet.message.size();
        }
    };

    static std::string _getPath(const std::string& filename) {
        uassert(ErrorCodes::BadValue,
                "Traffic recording filename must not be empty",
                !filename.empty());

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

    const std::string _path;
    const int64_t _maxLogSize;

    MultiProducerSingleConsumerQueue<TrafficRecordingPacket, CostFunction>::Pipe _pcqPipe;
    stdx::thread _thread;

    stdx::mutex _mutex;
    bool _inShutdown = false;
    TrafficRecorderStats _trafficStats;
    int64_t _written = 0;
    Status _result = Status::OK();
};

namespace {
static const auto getTrafficRecorder = ServiceContext::declareDecoration<TrafficRecorder>();
}  // namespace

TrafficRecorder& TrafficRecorder::get(ServiceContext* svc) {
    return getTrafficRecorder(svc);
}

TrafficRecorder::TrafficRecorder() : _shouldRecord(shouldAlwaysRecordTraffic) {}

TrafficRecorder::~TrafficRecorder() {
    if (shouldAlwaysRecordTraffic) {
        _recording->shutdown().ignore();
    }
}

void TrafficRecorder::start(const StartTrafficRecording& options) {
    invariant(!shouldAlwaysRecordTraffic);

    uassert(ErrorCodes::BadValue,
            "Traffic recording directory not set",
            !gTrafficRecordingDirectory.empty());

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        uassert(ErrorCodes::BadValue, "Traffic recording already active", !_recording);

        _recording = std::make_shared<Recording>(options);
        _recording->run();
    }

    _shouldRecord.store(true);
}

void TrafficRecorder::stop() {
    invariant(!shouldAlwaysRecordTraffic);

    _shouldRecord.store(false);

    auto recording = [&] {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        uassert(ErrorCodes::BadValue, "Traffic recording not active", _recording);

        return std::move(_recording);
    }();

    uassertStatusOK(recording->shutdown());
}

void TrafficRecorder::observe(const std::shared_ptr<transport::Session>& ts,
                              const Message& message,
                              ServiceContext* svcCtx) {
    if (shouldAlwaysRecordTraffic) {
        {
            stdx::lock_guard<stdx::mutex> lk(_mutex);

            if (!_recording) {
                StartTrafficRecording options;
                options.setDestination(gAlwaysRecordTraffic);
                options.setMaxFileSize({double(std::numeric_limits<int64_t>::max())});

                _recording = std::make_shared<Recording>(options);
                _recording->run();
            }
        }

        invariant(_recording->pushRecord(
            ts, svcCtx->getPreciseClockSource()->now(), _recording->order.addAndFetch(1), message));
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
    if (recording->pushRecord(
            ts, svcCtx->getPreciseClockSource()->now(), recording->order.addAndFetch(1), message)) {
        return;
    }

    // We couldn't queue
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // If the recording isn't the one we have in hand bail (its been ended, or a new one has
    // been created
    if (_recording != recording) {
        return;
    }

    // We couldn't queue and it's still our recording.  No one else should try to queue
    _shouldRecord.store(false);
}

std::shared_ptr<TrafficRecorder::Recording> TrafficRecorder::_getCurrentRecording() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _recording;
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

}  // namespace mongo
