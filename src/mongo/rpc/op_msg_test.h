// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/shared_buffer.h"

#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <utility>

#include <wiredtiger.h>


namespace mongo {
namespace rpc {
namespace test {

// Section bytes
constexpr char kBodySection = 0;
constexpr char kDocSequenceSection = 1;
constexpr char kTelemetrySection = 3;


// Makes a SharedBuffer out of arguments passed to constructor.
class Bytes {
public:
    template <typename... T>
    explicit Bytes(T&&... args) {
        append(args...);
    }

protected:
    void append() {}  // no-op base case

    template <typename T, typename... Rest>
    std::enable_if_t<std::is_integral<T>::value> append(T arg, Rest&&... rest) {
        // Make sure BufBuilder has a real overload of this exact type and it isn't implicitly
        // converted.
        (void)static_cast<void (BufBuilder::*)(T)>(&BufBuilder::appendNum);

        buffer.appendNum(arg);  // automatically little endian.
        append(rest...);
    }

    template <typename... Rest>
    void append(const BSONObj& arg, Rest&&... rest) {
        arg.appendSelfToBufBuilder(buffer);
        append(rest...);
    }

    template <typename... Rest>
    void append(const Bytes& arg, Rest&&... rest) {
        buffer.appendBuf(arg.buffer.buf(), arg.buffer.len());
        append(rest...);
    }

    template <typename... Rest>
    void append(std::string_view arg, Rest&&... rest) {
        buffer.appendCStr(arg);
        append(rest...);
    }

    BufBuilder buffer;
};

// A Bytes that puts the size of the buffer at the front as a little-endian int32
class Sized : public Bytes {
public:
    template <typename... T>
    explicit Sized(T&&... args) {
        buffer.skip(sizeof(int32_t));
        append(args...);
        updateSize();
    }

    // Adds extra to the stored size. Use this to produce illegal messages.
    Sized&& addToSize(int32_t extra) && {
        updateSize(extra);
        return std::move(*this);
    }

protected:
    void updateSize(int32_t extra = 0) {
        DataView(buffer.buf()).write<LittleEndian<int32_t>>(buffer.len() + extra);
    }
};

// A Bytes that puts the standard message header at the front.
class OpMsgBytes : public Sized {
public:
    template <typename... T>
    explicit OpMsgBytes(T&&... args)
        : Sized{int32_t{1},      // requestId
                int32_t{2},      // replyId
                int32_t{dbMsg},  // opCode
                args...} {}

    Message done() {
        const auto orig = Message(buffer.release());
        // Copy the message to an exact-sized allocation so ASAN can detect out-of-bounds accesses.
        auto copy = SharedBuffer::allocate(orig.size());
        memcpy(copy.get(), orig.buf(), orig.size());
        return Message(std::move(copy));
    }

    OpMsg parse() {
        return OpMsg::parseOwned(done());
    }

    OpMsg parse(Client* client) {
        return OpMsg::parseOwned(done(), client);
    }

    OpMsgBytes&& addToSize(int32_t extra) && {
        updateSize(extra);
        return std::move(*this);
    }

    OpMsgBytes&& appendChecksum() && {
        // Reserve space at the end for the checksum.
        append<uint32_t>(0);
        updateSize();
        // Checksum all bits except the checksum itself.
        uint32_t checksum = wiredtiger_crc32c_func()(buffer.buf(), buffer.len() - 4);
        // Write the checksum bits at the end.
        auto checksumBits = DataView(buffer.buf() + buffer.len() - sizeof(checksum));
        checksumBits.write<LittleEndian<uint32_t>>(checksum);
        return std::move(*this);
    }

    OpMsgBytes&& appendChecksum(uint32_t checksum) && {
        append(checksum);
        updateSize();
        return std::move(*this);
    }
};

}  // namespace test
}  // namespace rpc
}  // namespace mongo
