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

RecordingReader::RecordingReader(std::filesystem::path file)
    // Explicit boost::iostreams::detail::path required for MSVC and current version of boost to
    // compile.
    : RecordingReader(
          boost::iostreams::mapped_file_source(boost::iostreams::detail::path(file.string()))) {}
RecordingReader::RecordingReader(boost::iostreams::mapped_file_source mappedFile)
    : _mappedFile(mappedFile), _cdr(mappedFile.data(), mappedFile.size()) {
    invariant(_mappedFile.is_open());
}

boost::optional<TrafficReaderPacket> RecordingReader::readPacket() {
    if (_cdr.length() < 4) {
        return {};
    }
    auto len = _cdr.read<LittleEndian<uint32_t>>().value;

    uassert(ErrorCodes::FailedToParse, "packet too large", len < MaxMessageSizeBytes);
    uassert(ErrorCodes::FailedToParse, "could not read full packet", _cdr.length() >= len);
    auto packet = mongo::readPacket(_cdr.slice(size_t(len)));
    _cdr.advance(len);
    return packet;
}

}  // namespace mongo
