// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/replay/performance_reporter.h"

#include "mongo/base/data_builder.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/logv2/log.h"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <ios>
#include <string_view>
#include <vector>

namespace mongo {

template <typename Callable>
void handleErrors(Callable&& callable) {
    try {
        callable();
    } catch (const DBException& ex) {
        tasserted(ErrorCodes::ReplayClientSessionSimulationError,
                  "DBException in handleAsyncResponse, terminating due to:" + ex.toString());
    } catch (const std::exception& ex) {
        tasserted(ErrorCodes::ReplayClientSessionSimulationError,
                  "Exception in handleAsyncResponse, terminating due to:" + std::string{ex.what()});
    } catch (...) {
        tasserted(ErrorCodes::ReplayClientSessionSimulationError,
                  "Unknown exception in handleAsyncResponse, terminating");
    }
}

PerformanceReporter::PerformanceReporter(std::string_view uri,
                                         const std::string& perfFileName,
                                         size_t diskThreshold)
    : _packetsDumptoDiskThreshold(diskThreshold) {
    if (!perfFileName.empty()) {
        open(uri, perfFileName);
    }
}

PerformanceReporter::~PerformanceReporter() {
    _scheduler.join();
    close();
}

void PerformanceReporter::open(std::string_view uri, const std::string& filename) {
    _outFile.open(filename, std::ios::binary | std::ios::out | std::ios_base::trunc);
    uassert(ErrorCodes::ReplayClientInternalError,
            "Impossible to create performance report file for MongoR. Be sure that "
            "permissions have been set correctly.",
            _outFile);
    // the first bytes after we open the file is the mongouri.
    writeURI(uri);
}

void PerformanceReporter::close() {
    if (isPerfRecordingEnabled()) {
        write(_packets);
        _packets.clear();
        _outFile.close();
    }
}

BSONObj PerformanceReporter::executeAndRecordPerf(ExecutionCallback&& f,
                                                  const ReplayCommand& command) {
    if (!isPerfRecordingEnabled()) {
        // no perf recording involved, just execute the lambda (which should run the command).
        return f(command);
    }
    // compute the information needed and store on file the perf recorded.
    const auto start = std::chrono::high_resolution_clock::now();
    const auto resp = f(command);
    const auto end = std::chrono::high_resolution_clock::now();
    const auto sessionId = command.fetchRequestSessionId();
    const auto messageId = command.fetchMessageId();
    const auto ncount = extractNumberOfDocuments(resp);
    const auto duration =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    add(PerformancePacket{sessionId, messageId, duration, ncount});
    return resp;
}

void PerformanceReporter::add(const PerformancePacket& packet) {
    if (!isPerfRecordingEnabled()) {
        return;
    }
    _packets.push_back(packet);
    if (_packets.size() == _packetsDumptoDiskThreshold) {
        auto f = [this, packets = std::move(_packets)]() {
            handleErrors([this, &packets] { write(packets); });
        };
        _scheduler.submit((f));
    }
}

void PerformanceReporter::write(const std::vector<PerformancePacket>& packets) {
    // There is only one bg thread for writing to disk. Moreover by design the recording file is
    // opened when the performance reporter is created and closed when the object is destroyed. So
    // there cannot be any data races.
    uassert(ErrorCodes::ReplayClientInternalError,
            "Impossible to write the packets in the performance file. Check permissions!",
            _outFile);
    for (const auto& packet : packets) {
        writePacket(packet);
    }
}

PerformanceRecording PerformanceReporter::read(const std::string& fileName) {
    std::ifstream inputFile(fileName, std::ios::binary | std::ios::in);
    uassert(ErrorCodes::ReplayClientInternalError,
            fmt::format("Failed to open perf file: {}", fileName),
            inputFile);

    // A recording perf file is stored in this format:
    // <mongoURI><packet1><packet2>....<packetN>

    // first read the length of the mongoURI
    const auto mongoURI = readURI(inputFile);
    // then process the packets
    std::vector<PerformancePacket> packets;
    while (inputFile.peek() != EOF) {
        packets.push_back(readPacket(inputFile));
    }
    return PerformanceRecording{mongoURI, packets};
}

void PerformanceReporter::writePacket(const PerformancePacket& packet) {
    DataBuilder db;
    uassertStatusOK(db.writeAndAdvance<LittleEndian<uint64_t>>(packet.sessionId));
    uassertStatusOK(db.writeAndAdvance<LittleEndian<uint64_t>>(packet.messageId));
    uassertStatusOK(db.writeAndAdvance<LittleEndian<int64_t>>(packet.time));
    uassertStatusOK(db.writeAndAdvance<LittleEndian<uint64_t>>(packet.ncount));
    _outFile.write(db.getCursor().data(), db.size());
    uassert(ErrorCodes::ReplayClientInternalError,
            "Writing down perf record to disk has failed!",
            _outFile);
}

PerformancePacket PerformanceReporter::readPacket(std::ifstream& inFile) {
    static const auto kMessageLen = sizeof(PerformancePacket);
    std::array<char, kMessageLen> buf;  // Stack-based array
    inFile.read(buf.data(), kMessageLen);
    uassert(ErrorCodes::ReplayClientInternalError,
            fmt::format("Reading perf record failed at offset:{}", (size_t)inFile.tellg()),
            !(inFile.fail() || inFile.eof()));
    ConstDataRangeCursor cdr(buf.data(), buf.data() + buf.size());
    uint64_t sessionId = cdr.readAndAdvance<LittleEndian<uint64_t>>();
    uint64_t messageId = cdr.readAndAdvance<LittleEndian<uint64_t>>();
    int64_t time = cdr.readAndAdvance<LittleEndian<int64_t>>();
    uint64_t ncount = cdr.readAndAdvance<LittleEndian<uint64_t>>();

    // Reminder to update readPacket/writePacket if fields are added to PerformancePacket.
    static_assert(sizeof(PerformancePacket) == sizeof(uint64_t) * 4);

    return PerformancePacket{sessionId, messageId, time, ncount};
}

uint64_t PerformanceReporter::extractNumberOfDocuments(const BSONObj& response) {
    if (response.hasField("n")) {
        return response["n"].Int();
    } else if (response.hasField("cursor")) {
        BSONObj cursor = response["cursor"].Obj();
        if (cursor.hasField("firstBatch")) {
            const auto& firstBatch = cursor["firstBatch"].Array();
            return firstBatch.size();
        }
    }
    // by default return 0 docs when we receive a response that we don't know.
    return 0;
}

std::string PerformanceReporter::readURI(std::ifstream& inFile) {
    std::string_view::size_type uriLen = 0;
    inFile.read((char*)(&uriLen), sizeof(uriLen));
    uassert(ErrorCodes::ReplayClientInternalError,
            "Reading perf file URI length header failed",
            !(inFile.fail() || inFile.eof()));
    if (!uriLen) {
        return {};
    }
    std::string buf;
    buf.resize(uriLen);
    inFile.read(buf.data(), uriLen);
    uassert(ErrorCodes::ReplayClientInternalError,
            "Reading perf file URI failed",
            !(inFile.fail() || inFile.eof()));
    return buf;
}

void PerformanceReporter::writeURI(std::string_view uri) {
    DataBuilder db;
    uassertStatusOK(db.writeAndAdvance<LittleEndian<std::string_view::size_type>>(uri.size()));
    uassertStatusOK(db.writeAndAdvance<std::string_view>(uri));
    _outFile.write(db.getCursor().data(), db.size());
}

bool PerformanceReporter::isPerfRecordingEnabled() const {
    return _outFile && _outFile.is_open();
}
}  // namespace mongo
