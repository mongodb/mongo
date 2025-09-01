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

#include "mongo/replay/performance_reporter.h"

#include "mongo/base/data_builder.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/mutex.h"

#include <cstdlib>
#include <exception>
#include <fstream>
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

PerformanceReporter::PerformanceReporter(StringData uri,
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

void PerformanceReporter::open(StringData uri, const std::string& filename) {
    _outFile.open(filename, std::ios::binary | std::ios::out);
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
        _outFile.close();
    }
}

void PerformanceReporter::executeAndRecordPerf(ExecutionCallback&& f,
                                               const ReplayCommand& command) {
    if (!isPerfRecordingEnabled()) {
        // no perf recording involved, just execute the lambda (which should run the command).
        f(command);
        return;
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
            fmt::format("Impossible to read the content for the file: {}", fileName),
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
    const auto messageLen = sizeof(PerformancePacket);
    std::array<char, messageLen> buf;  // Stack-based array
    inFile.read(buf.data(), messageLen);
    uassert(ErrorCodes::ReplayClientInternalError,
            "Reading perf record from disk has failed (invalid input file).",
            inFile);
    ConstDataRangeCursor cdr(buf.data(), buf.data() + buf.size());
    uint64_t sessionId = cdr.readAndAdvance<LittleEndian<uint64_t>>();
    uint64_t messageId = cdr.readAndAdvance<LittleEndian<uint64_t>>();
    int64_t time = cdr.readAndAdvance<LittleEndian<int64_t>>();
    uint64_t ncount = cdr.readAndAdvance<LittleEndian<uint64_t>>();
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
    StringData::size_type uriLen;
    inFile.read((char*)(&uriLen), sizeof(uriLen));
    uassert(ErrorCodes::ReplayClientInternalError,
            "Reading perf record from disk has failed (invalid uri len).",
            uriLen != 0);
    std::string buf;
    buf.resize(uriLen);
    inFile.read(buf.data(), uriLen);
    auto uri = ConstDataRangeCursor(buf.data(), buf.data() + uriLen).readAndAdvance<StringData>();
    uassert(ErrorCodes::ReplayClientInternalError,
            "Reading perf record from disk has failed (mongo uri is not correct).",
            uri.size() == uriLen);
    return std::string(uri);
}

void PerformanceReporter::writeURI(StringData uri) {
    DataBuilder db;
    uassertStatusOK(db.writeAndAdvance<LittleEndian<StringData::size_type>>(uri.size()));
    uassertStatusOK(db.writeAndAdvance<StringData>(uri));
    _outFile.write(db.getCursor().data(), db.size());
}

bool PerformanceReporter::isPerfRecordingEnabled() const {
    return _outFile && _outFile.is_open();
}
}  // namespace mongo
