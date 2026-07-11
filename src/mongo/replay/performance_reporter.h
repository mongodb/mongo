// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/replay/replay_command.h"
#include "mongo/replay/session_scheduler.h"
#include "mongo/util/modules.h"

#include <fstream>
#include <string_view>

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
    explicit PerformanceReporter(std::string_view uri,
                                 const std::string& perfFileName = "",
                                 size_t diskThreshold = DUMP_TO_DISK_THRESHOLD);
    ~PerformanceReporter();
    BSONObj executeAndRecordPerf(ExecutionCallback&&, const ReplayCommand&);
    // addPacket assumes that there is only one bg thread that is responsible of writing down to
    // disk the performance information recorded. Thus no locking is needed, since all the work is
    // scheduled via the SessionScheduler (which uses a simple producer/consumer model).
    void add(const PerformancePacket&);
    static PerformanceRecording read(const std::string&);

private:
    // open is called inside the PerformanceReporter ctor. PerformanceReporter abides to RAII
    // pattern. The file is created before any other bg thread can touch it. Thus no locking is
    // needed.
    void open(std::string_view uri, const std::string& filename);
    // close is designed to be called inside PerformanceReporter dtor (RAII pattern). Which implies
    // that all the bg threads will be joined before closing the file, and all the working threads
    // will have written all the data. For this reason no locking mechanism is needed.
    void close();
    void write(const std::vector<PerformancePacket>&);
    void writePacket(const PerformancePacket&);
    void writeURI(std::string_view);
    bool isPerfRecordingEnabled() const;
    static PerformancePacket readPacket(std::ifstream& inFile);
    static std::string readURI(std::ifstream& inFile);
    static uint64_t extractNumberOfDocuments(const BSONObj& response);

    size_t _packetsDumptoDiskThreshold{DUMP_TO_DISK_THRESHOLD};
    SessionScheduler _scheduler;
    std::ofstream _outFile;
    std::vector<PerformancePacket> _packets;
};
}  // namespace mongo
