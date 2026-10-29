// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/traffic_recorder.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/modules.h"

#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>

#pragma once

namespace mongo {
static const std::string kSessionStartOpType = "sessionStart";
static const std::string kSessionEndOpType = "sessionEnd";

// Packet struct
struct TrafficReaderPacket {
    EventType eventType;
    uint64_t id;
    std::string_view session;
    Microseconds offset;  // offset from the start of the recording in microseconds
    uint64_t order;
    MsgData::ConstView message;
};

// Comparator for round-trip testing that a packet read from disk is equal to the value written to
// disk originally.
bool operator==(const TrafficReaderPacket& read, const TrafficRecordingPacket& recorded);

// Method for testing, takes the recorded traffic and returns a BSONArray
[[MONGO_MOD_PUBLIC]] BSONArray trafficRecordingFileToBSONArr(const std::string& inputFile,
                                                             bool bodyAsNestedDoc = false);

// This is the function that traffic_reader_main.cpp calls
void trafficRecordingFileToMongoReplayFile(int inFile, std::ostream& outFile);

TrafficReaderPacket readPacket(ConstDataRangeCursor cdr);
}  // namespace mongo
