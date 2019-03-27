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

#include "mongo/platform/basic.h"

#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/types.h>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "mongo/base/data_cursor.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_type_validated.h"
#include "mongo/base/data_view.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/traffic_reader.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

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

namespace {

// Packet struct
struct TrafficReaderPacket {
    uint64_t id;
    StringData local;
    StringData remote;
    Date_t date;
    uint64_t order;
    MsgData::ConstView message;
};

bool readBytes(size_t toRead, char* buf, int fd) {
    while (toRead) {
#ifdef _WIN32
        auto r = _read(fd, buf, toRead);
#else
        auto r = ::read(fd, buf, toRead);
#endif

        if (r == -1) {
            auto pair = errnoAndDescription();

            uassert(ErrorCodes::FileStreamFailed,
                    str::stream() << "failed to read bytes: errno(" << pair.first << ") : "
                                  << pair.second,
                    pair.first == EINTR);

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

    ConstDataRangeCursor cdr(buf, buf + len);

    // Read the packet
    cdr.skip<LittleEndian<uint32_t>>();
    uint64_t id = cdr.readAndAdvance<LittleEndian<uint64_t>>();
    StringData local = cdr.readAndAdvance<Terminated<'\0', StringData>>();
    StringData remote = cdr.readAndAdvance<Terminated<'\0', StringData>>();
    uint64_t date = cdr.readAndAdvance<LittleEndian<uint64_t>>();
    uint64_t order = cdr.readAndAdvance<LittleEndian<uint64_t>>();
    MsgData::ConstView message(cdr.data());

    return TrafficReaderPacket{
        id, local, remote, Date_t::fromMillisSinceEpoch(date), order, message};
}

void getBSONObjFromPacket(TrafficReaderPacket& packet, BSONObjBuilder* builder) {
    {
        // RawOp Field
        BSONObjBuilder rawop(builder->subobjStart("rawop"));

        // Add the header fields to rawOp
        {
            BSONObjBuilder header(rawop.subobjStart("header"));
            header.append("messagelength", static_cast<int32_t>(packet.message.getLen()));
            header.append("requestid", static_cast<int32_t>(packet.message.getId()));
            header.append("responseto", static_cast<int32_t>(packet.message.getResponseToMsgId()));
            header.append("opcode", static_cast<int32_t>(packet.message.getNetworkOp()));
        }

        // Add the binary reprentation of the entire message for rawop.body
        // auto buf = SharedBuffer::allocate(packet.message.getLen());
        // std::memcpy(buf.get(), packet.message.view2ptr(), packet.message.getLen());
        // rawop.appendBinData("body", packet.message.getLen(), BinDataGeneral, buf.get());
        rawop.appendBinData(
            "body", packet.message.getLen(), BinDataGeneral, packet.message.view2ptr());
    }

    // The seen field represents the time that the operation took place
    // Trying to re-create the way mongoreplay does this
    {
        BSONObjBuilder seen(builder->subobjStart("seen"));
        seen.append(
            "sec",
            static_cast<int64_t>((packet.date.toMillisSinceEpoch() / 1000) + unixToInternal));
        seen.append("nsec", static_cast<int32_t>(packet.order));
    }

    // Figure out which is the src endpoint as opposed to the dest endpoint
    auto localInd = packet.local.rfind(':');
    auto remoteInd = packet.remote.rfind(':');
    if (localInd != std::string::npos && remoteInd != std::string::npos) {
        auto local = packet.local.substr(localInd + 1);
        auto remote = packet.remote.substr(remoteInd + 1);
        if (packet.message.getResponseToMsgId()) {
            builder->append("srcendpoint", local);
            builder->append("destendpoint", remote);
        } else {
            builder->append("srcendpoint", remote);
            builder->append("destendpoint", local);
        }
    }

    // Fill out the remaining fields
    builder->append("order", static_cast<int64_t>(packet.order));
    builder->append("seenconnectionnum", static_cast<int64_t>(packet.id));
    builder->append("playedconnectionnum", static_cast<int64_t>(0));
    builder->append("generation", static_cast<int32_t>(0));
}

void addOpType(TrafficReaderPacket& packet, BSONObjBuilder* builder) {
    if (packet.message.getNetworkOp() == dbMsg) {
        Message message;
        message.setData(dbMsg, packet.message.data(), packet.message.dataLen());

        auto opMsg = rpc::opMsgRequestFromAnyProtocol(message);
        builder->append("opType", opMsg.getCommandName());
    } else {
        builder->append("opType", "legacy");
    }
}

}  // namespace

BSONArray trafficRecordingFileToBSONArr(const std::string& inputFile) {
    BSONArrayBuilder builder{};

// Open the connection to the input file
#ifdef _WIN32
    auto inputFd = ::open(inputFile.c_str(), O_RDONLY | O_BINARY);
#else
    auto inputFd = ::open(inputFile.c_str(), O_RDONLY);
#endif

    uassert(ErrorCodes::FileNotOpen,
            str::stream() << "Specified file does not exist (" << inputFile << ")",
            inputFd > 0);

    const auto guard = makeGuard([&] { ::close(inputFd); });

    auto buf = SharedBuffer::allocate(MaxMessageSizeBytes);
    while (auto packet = readPacket(buf.get(), inputFd)) {
        BSONObjBuilder bob(builder.subobjStart());
        getBSONObjFromPacket(*packet, &bob);
        addOpType(*packet, &bob);
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
