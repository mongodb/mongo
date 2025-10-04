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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/traffic_recorder.h"
#include "mongo/rpc/op_msg.h"

#include <filesystem>
#include <iosfwd>
#include <string>

#pragma once

namespace mongo {
static const std::string kSessionStartOpType = "sessionStart";
static const std::string kSessionEndOpType = "sessionEnd";

// Packet struct
struct TrafficReaderPacket {
    EventType eventType;
    uint64_t id;
    StringData session;
    Microseconds offset;  // offset from the start of the recording in microseconds
    uint64_t order;
    MsgData::ConstView message;
};

// Comparator for round-trip testing that a packet read from disk is equal to the value written to
// disk originally.
bool operator==(const TrafficReaderPacket& read, const TrafficRecordingPacket& recorded);

// Method for testing, takes the recorded traffic and returns a BSONArray
BSONArray trafficRecordingFileToBSONArr(const std::string& inputFile);

// This is the function that traffic_reader_main.cpp calls
void trafficRecordingFileToMongoReplayFile(int inFile, std::ostream& outFile);

TrafficReaderPacket readPacket(ConstDataRangeCursor cdr);
}  // namespace mongo
