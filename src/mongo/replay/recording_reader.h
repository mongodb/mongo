// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/traffic_reader.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/iostreams/device/mapped_file.hpp>

namespace mongo {

class RecordingReader {
public:
    RecordingReader() = default;
    RecordingReader(std::filesystem::path file);
    RecordingReader(boost::iostreams::mapped_file_source mappedFile);
    boost::optional<TrafficReaderPacket> readPacket();

private:
    boost::iostreams::mapped_file_source _mappedFile;
    ConstDataRangeCursor _cdr;
};

}  // namespace mongo
