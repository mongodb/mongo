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

#include "mongo/util/duration.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>

#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#ifdef _WIN32
#include <io.h>
#endif

#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_type_terminated.h"
#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/traffic_reader.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

namespace {
// Taken from src/mongo/gotools/mongoreplay/util.go
// Time.Unix() returns the number of seconds from the unix epoch but time's
// underlying struct stores 'sec' as the number of seconds elapsed since
// January 1, year 1 00:00:00 UTC (In the Proleptic Gregorian Calendar)
// This calculation allows for conversion between the internal representation
// and the UTC representation.
const long long unixToInternal =
    static_cast<long long>(1969 * 365 + 1969 / 4 - 1969 / 100 + 1969 / 400) * 86400;
}  // namespace

namespace mongo {

TrafficReaderPacket readPacket(ConstDataRangeCursor cdr) {
    // Read the packet
    cdr.skip<LittleEndian<uint32_t>>();
    EventType eventType = cdr.readAndAdvance<EventType>();
    uint64_t id = cdr.readAndAdvance<LittleEndian<uint64_t>>();
    StringData session = cdr.readAndAdvance<Terminated<'\0', StringData>>();
    int64_t offsetMicrosCount = cdr.readAndAdvance<LittleEndian<uint64_t>>();
    Microseconds offset{offsetMicrosCount};
    uint64_t order = cdr.readAndAdvance<LittleEndian<uint64_t>>();
    MsgData::ConstView message(cdr.data());
    tassert(10562701,
            "The value of 'eventType' can only be a value of existent event type",
            eventType < EventType::kMax);

    return TrafficReaderPacket{eventType, id, session, offset, order, message};
}

namespace {
bool readBytes(size_t toRead, char* buf, int fd) {
    while (toRead) {
#ifdef _WIN32
        auto r = _read(fd, buf, toRead);
#else
        auto r = ::read(fd, buf, toRead);
#endif

        if (r == -1) {
            auto ec = lastPosixError();
            uassert(ErrorCodes::FileStreamFailed,
                    str::stream() << "failed to read bytes: errno(" << ec.value()
                                  << ") : " << errorMessage(ec),
                    ec == posixError(EINTR));

            continue;
        } else if (r == 0) {
            return false;
        }

        buf += r;
        toRead -= r;
    }

    return true;
}

boost::optional<TrafficReaderPacket> readPacket(char* buf, int fd) {
    if (!readBytes(4, buf, fd)) {
        return boost::none;
    }
    auto len = ConstDataView(buf).read<LittleEndian<uint32_t>>();

    uassert(ErrorCodes::FailedToParse, "packet too large", len < MaxMessageSizeBytes);
    uassert(
        ErrorCodes::FailedToParse, "could not read full packet", readBytes(len - 4, buf + 4, fd));

    return readPacket(ConstDataRangeCursor(buf, buf + len));
}

void getBSONObjFromPacket(TrafficReaderPacket& packet, BSONObjBuilder* builder) {
    {
        // RawOp Field
        BSONObjBuilder rawop(builder->subobjStart("rawop"));

        // Some special events like session events don't have a Message, so these fields can be
        // optional.
        if (packet.eventType == EventType::kRegular) {
            // Add the header fields to rawOp
            {
                BSONObjBuilder header(rawop.subobjStart("header"));
                header.append("messagelength", static_cast<int32_t>(packet.message.getLen()));
                header.append("requestid", static_cast<int32_t>(packet.message.getId()));
                header.append("responseto",
                              static_cast<int32_t>(packet.message.getResponseToMsgId()));
                header.append("opcode", static_cast<int32_t>(packet.message.getNetworkOp()));
            }

            rawop.appendBinData(
                "body", packet.message.getLen(), BinDataGeneral, packet.message.view2ptr());
        }
    }

    builder->append("event", static_cast<int32_t>(packet.eventType));
    builder->append("session", packet.session);
    builder->append("offset", durationCount<Microseconds>(packet.offset));
    builder->append("order", static_cast<int64_t>(packet.order));
    builder->append("seenconnectionnum", static_cast<int64_t>(packet.id));
    builder->append("playedconnectionnum", static_cast<int64_t>(0));
    builder->append("generation", static_cast<int32_t>(0));
}

void addOpType(TrafficReaderPacket& packet, BSONObjBuilder* builder) {
    if (packet.eventType == EventType::kSessionStart) {
        builder->append("opType", kSessionStartOpType);
        return;
    }
    if (packet.eventType == EventType::kSessionEnd) {
        builder->append("opType", kSessionEndOpType);
        return;
    }
    if (packet.message.getNetworkOp() == dbMsg) {
        Message message;
        message.setData(dbMsg, packet.message.data(), packet.message.dataLen());
        // Some header fields like requestId are missing, so the checksum won't match.
        OpMsg::removeChecksum(&message);
        auto opMsg = rpc::opMsgRequestFromAnyProtocol(message);
        builder->append("opType", opMsg.getCommandName());
    } else {
        builder->append("opType", "legacy");
    }
}

}  // namespace

bool operator==(const TrafficReaderPacket& read, const TrafficRecordingPacket& recorded) {
    std::string_view readData(read.message.data(), read.message.dataLen());
    MsgData::ConstView recordedView(recorded.message.buf());
    std::string_view recordedData(recordedView.data(), recordedView.dataLen());
    return std::tie(read.id, read.session, read.offset, read.order, readData) ==
        std::tie(recorded.id, recorded.session, recorded.offset, recorded.order, recordedData);
}

BSONArray trafficRecordingFileToBSONArr(const std::string& inputFile) {
    BSONArrayBuilder builder{};

    uassert(ErrorCodes::FileNotOpen,
            str::stream() << "Specified file/directory does not exist (" << inputFile << ")",
            std::filesystem::exists(inputFile));

    std::vector<std::string> files;

    if (std::filesystem::is_directory(inputFile)) {
        for (const auto& entry : std::filesystem::directory_iterator{inputFile}) {
            if (!entry.is_regular_file()) {
                continue;
            }
            if (entry.path().extension() != ".bin") {
                continue;
            }
            files.push_back(entry.path().string());
        }
        std::sort(files.begin(), files.end());
    } else {
        files.push_back(inputFile);
    }

    auto buf = SharedBuffer::allocate(MaxMessageSizeBytes);

    for (const auto& file : files) {
// Open the connection to the input file
#ifdef _WIN32
        auto inputFd = ::open(file.c_str(), O_RDONLY | O_BINARY);
#else
        auto inputFd = ::open(file.c_str(), O_RDONLY);
#endif

        uassert(ErrorCodes::FileNotOpen,
                str::stream() << "Specified file does not exist (" << file << ")",
                inputFd > 0);

        const ScopeGuard guard([&] { ::close(inputFd); });

        while (auto packet = readPacket(buf.get(), inputFd)) {
            BSONObjBuilder bob(builder.subobjStart());
            getBSONObjFromPacket(*packet, &bob);
            addOpType(*packet, &bob);
        }
    }

    return builder.arr();
}

void trafficRecordingFileToMongoReplayFile(int inputFd, std::ostream& outputStream) {
    // Document expected by mongoreplay
    BSONObjBuilder opts{};
    opts.append("playbackfileversion", 1);
    opts.append("driveropsfiltered", false);
    auto optsObj = opts.obj();
    outputStream.write(optsObj.objdata(), optsObj.objsize());

    BSONObjBuilder bob;
    auto buf = SharedBuffer::allocate(MaxMessageSizeBytes);

    while (auto packet = readPacket(buf.get(), inputFd)) {
        getBSONObjFromPacket(*packet, &bob);

        auto obj = bob.asTempObj();
        outputStream.write(obj.objdata(), obj.objsize());

        bob.resetToEmpty();
    }
}

}  // namespace mongo
