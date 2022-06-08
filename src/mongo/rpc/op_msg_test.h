/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <type_traits>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/rpc/op_msg.h"
#include "third_party/wiredtiger/wiredtiger.h"

namespace mongo {
namespace rpc {
namespace test {

// Section bytes
constexpr char kBodySection = 0;
constexpr char kDocSequenceSection = 1;
constexpr char kSecurityTokenSection = 2;

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
    void append(StringData arg, Rest&&... rest) {
        buffer.appendStr(arg, /* null terminate*/ true);
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
