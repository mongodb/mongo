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

#include "mongo/base/data_range_cursor.h"
#include "mongo/db/traffic_reader.h"
#include "mongo/util/assert_util.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <unordered_set>
#include <vector>

namespace mongo {

std::vector<TrafficReaderPacket> RecordingReader::processRecording() {
    size_t size = 0;
    std::ifstream f;

    try {
        size = std::filesystem::file_size(filename);
        f.exceptions(std::ifstream::badbit | std::ifstream::failbit);
        f.open(filename, std::ios_base::in | std::ios_base::binary);
    } catch (const std::exception&) {
        uasserted(ErrorCodes::FileOpenFailed, "Couldn't open recording file");
    }

    buffer = std::make_unique<char[]>(size);
    f.read(buffer.get(), size);

    std::vector<TrafficReaderPacket> res;

    ConstDataRangeCursor cdr(buffer.get(), size);

    while (auto packet = maybeReadPacket(cdr)) {
        res.push_back(*packet);
    }

    return res;
}
}  // namespace mongo
