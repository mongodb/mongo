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

#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/platform/atomic.h"
#include "mongo/replay/replay_command.h"
#include "mongo/replay/session_scheduler.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/modules.h"

#include <fstream>
#include <tuple>

namespace mongo {

static constexpr auto DUMP_TO_DISK_THRESHOLD = 10;
struct PerformancePacket {
    uint64_t sessionId;
    uint64_t messageId;
    int64_t time;
    uint64_t ncount;

    PerformancePacket() = default;
    PerformancePacket(uint64_t sessionId, uint64_t messageId, int64_t time, uint64_t ncount)
        : sessionId(sessionId), messageId(messageId), time(time), ncount(ncount) {}

    friend bool operator==(const PerformancePacket& lhs, const PerformancePacket& rhs) = default;
};

struct PerformanceRecording {
    std::string mongoURI;
    std::vector<PerformancePacket> packets;
};

class PerformanceReporter {
public:
    using ExecutionCallback = mongo::unique_function<BSONObj(const ReplayCommand&)>;
    explicit PerformanceReporter(StringData uri,
                                 const std::string& perfFileName = "",
                                 size_t diskThreshold = DUMP_TO_DISK_THRESHOLD);
    ~PerformanceReporter();
    void executeAndRecordPerf(ExecutionCallback&&, const ReplayCommand&);
    // addPacket assumes that there is only one bg thread that is responsible of writing down to
    // disk the performance information recorded. Thus no locking is needed, since all the work is
    // scheduled via the SessionScheduler (which uses a simple producer/consumer model).
    void add(const PerformancePacket&);
    static PerformanceRecording read(const std::string&);

private:
    // open is called inside the PerformanceReporter ctor. PerformanceReporter abides to RAII
    // pattern. The file is created before any other bg thread can touch it. Thus no locking is
    // needed.
    void open(StringData uri, const std::string& filename);
    // close is designed to be called inside PerformanceReporter dtor (RAII pattern). Which implies
    // that all the bg threads will be joined before closing the file, and all the working threads
    // will have written all the data. For this reason no locking mechanism is needed.
    void close();
    void write(const std::vector<PerformancePacket>&);
    void writePacket(const PerformancePacket&);
    void writeURI(StringData);
    bool isPerfRecordingEnabled() const;
    static std::string toFileName(StringData);
    static PerformancePacket readPacket(std::ifstream& inFile);
    static std::string readURI(std::ifstream& inFile);
    static uint64_t extractNumberOfDocuments(const BSONObj& response);

    size_t _packetsDumptoDiskThreshold{DUMP_TO_DISK_THRESHOLD};
    SessionScheduler _scheduler;
    std::ofstream _outFile;
    std::vector<PerformancePacket> _packets;
};
}  // namespace mongo
