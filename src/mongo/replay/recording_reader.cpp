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
#include "mongo/replay/recording_reader.h"

#include "mongo/db/traffic_reader.h"

#include <string>
#include <unordered_set>

namespace mongo {

static std::unordered_set<std::string> forbiddenKeywords{
    "legacy", "cursor", "endSessions", "ok", "isWritablePrimary", "n"};

inline bool isReplayable(const std::string& commandType) {
    return !commandType.empty() && !forbiddenKeywords.contains(commandType);
}

std::vector<BSONObj> RecordingReader::parse() const {

    // recording format.
    // {
    //   "rawop": {
    //     "header": {
    //       "messagelength": 120,
    //       "requestid": 3,
    //       "responseto": 0,
    //       "opcode": 2013
    //     },
    //     "body": "BinData(0, ...)"
    //   },
    //   "seen": {
    //     "sec": 63883941272,
    //     "nsec": 8
    //   },
    //   "session": {
    //     "remote": "127.0.0.1:54482",
    //     "local": "127.0.0.1:27017"
    //   },
    //   "order": 8,
    //   "seenconnectionnum": 3,
    //   "playedconnectionnum": 0,
    //   "generation": 0,
    //   "opType": "find"
    // }

    std::vector<BSONObj> res;
    BSONArray array = trafficRecordingFileToBSONArr(filename);
    BSONObjIterator it(array);
    while (it.more()) {
        BSONElement elem = it.next();

        std::string opType;
        BSONElement opTypeElem = elem["opType"];
        if (opTypeElem.type() == mongo::BSONType::string) {
            opType = opTypeElem.String();
        }

        // skip all the messages we are neither able to parse
        // nor we care about (for now)
        if (!isReplayable(opType))
            continue;

        BSONElement rawopElem = elem["rawop"];
        if (rawopElem.type() == mongo::BSONType::object) {
            // leave the interpretation of the binary data to replay command
            res.push_back(rawopElem.Obj().copy());
        }
    }
    return res;
}
}  // namespace mongo
