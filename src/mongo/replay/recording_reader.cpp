// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
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
