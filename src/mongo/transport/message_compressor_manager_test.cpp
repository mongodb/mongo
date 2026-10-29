// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/transport/message_compressor_manager.h"

#include "mongo/base/data_range.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_compressed.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/message_compressor_noop.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/transport/message_compressor_snappy.h"
#include "mongo/transport/message_compressor_zlib.h"
#include "mongo/transport/message_compressor_zstd.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/str.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

const auto assertOk = [](auto&& sw) {
    ASSERT(sw.isOK());
    return sw.getValue();
};

MessageCompressorRegistry buildRegistry() {
    MessageCompressorRegistry ret;
    auto compressor = std::make_unique<NoopMessageCompressor>();

    std::vector<std::string> compressorList = {compressor->getName()};
    ret.setSupportedCompressors(std::move(compressorList));
    ret.registerImplementation(std::move(compressor));
    ret.finalizeSupportedCompressors().transitional_ignore();

    return ret;
}

void checkNegotiationResult(const BSONObj& result, const std::vector<std::string>& algos) {
    auto compressorsList = result.getField("compression");
    if (algos.empty()) {
        ASSERT_TRUE(compressorsList.eoo());
        return;
    }
    ASSERT_TRUE(!compressorsList.eoo());
    ASSERT_TRUE(compressorsList.isABSONObj());
    auto compressorsListObj = compressorsList.Obj();

    std::vector<std::string> resultAlgos;
    for (const auto& e : compressorsListObj) {
        resultAlgos.push_back(std::string{e.checkAndGetStringData()});
    }
    ASSERT_EQ(algos.size(), resultAlgos.size());
    for (size_t i = 0; i < algos.size(); i++) {
        ASSERT_EQ(algos[i], resultAlgos[i]);
    }
}

void checkServerNegotiation(const boost::optional<std::vector<std::string_view>>& input,
                            const std::vector<std::string>& expected) {
    auto registry = buildRegistry();
    MessageCompressorManager manager(&registry);

    BSONObjBuilder serverOutput;
    manager.serverNegotiate(input, &serverOutput);
    checkNegotiationResult(serverOutput.done(), expected);
}

void checkFidelity(const Message& msg, std::unique_ptr<MessageCompressorBase> compressor) {
    MessageCompressorRegistry registry;
    const auto originalView = msg.singleData();
    const auto compressorName = compressor->getName();

    std::vector<std::string> compressorList = {compressorName};
    registry.setSupportedCompressors(std::move(compressorList));
    registry.registerImplementation(std::move(compressor));
    registry.finalizeSupportedCompressors().transitional_ignore();

    MessageCompressorManager mgr(&registry);
    BSONObjBuilder negotiatorOut;
    std::vector<std::string_view> negotiator({compressorName});
    mgr.serverNegotiate(negotiator, &negotiatorOut);
    checkNegotiationResult(negotiatorOut.done(), {compressorName});

    auto swm = mgr.compressMessage(msg);
    ASSERT_OK(swm.getStatus());
    auto compressedMsg = std::move(swm.getValue());
    const auto compressedMsgView = compressedMsg.singleData();

    ASSERT_EQ(compressedMsgView.getId(), originalView.getId());
    ASSERT_EQ(compressedMsgView.getResponseToMsgId(), originalView.getResponseToMsgId());
    ASSERT_EQ(compressedMsgView.getNetworkOp(), dbCompressed);

    swm = mgr.decompressMessage(compressedMsg);
    ASSERT_OK(swm.getStatus());
    auto decompressedMsg = std::move(swm.getValue());

    const auto decompressedMsgView = decompressedMsg.singleData();
    ASSERT_EQ(decompressedMsgView.getId(), originalView.getId());
    ASSERT_EQ(decompressedMsgView.getResponseToMsgId(), originalView.getResponseToMsgId());
    ASSERT_EQ(decompressedMsgView.getNetworkOp(), originalView.getNetworkOp());
    ASSERT_EQ(decompressedMsgView.getLen(), originalView.getLen());

    ASSERT_EQ(memcmp(decompressedMsgView.data(), originalView.data(), originalView.dataLen()), 0);
}

void checkOverflow(std::unique_ptr<MessageCompressorBase> compressor) {
    // This is our test data that we're going to try to compress/decompress into a buffer that's
    // way too small.
    const std::string data =
        "We embrace reality. We apply high-quality thinking and rigor."
        "We have courage in our convictions but work hard to ensure biases "
        "or personal beliefs do not get in the way of finding the best solution.";
    ConstDataRange input(data.data(), data.size());

    // This is our tiny buffer that should cause an error.
    std::array<char, 16> smallBuffer;
    DataRange smallOutput(smallBuffer.data(), smallBuffer.size());

    // This is a normal sized buffer that we can store a compressed version of our test data safely
    std::vector<char> normalBuffer;
    normalBuffer.resize(compressor->getMaxCompressedSize(data.size()));
    auto sws = compressor->compressData(input, DataRange(normalBuffer.data(), normalBuffer.size()));
    ASSERT_OK(sws);
    DataRange normalRange = DataRange(normalBuffer.data(), sws.getValue());

    // Check that compressing the test data into a small buffer fails
    ASSERT_NOT_OK(compressor->compressData(input, smallOutput));

    // Check that decompressing compressed test data into a small buffer fails
    ASSERT_NOT_OK(compressor->decompressData(normalRange, smallOutput));

    // Check that decompressing a valid buffer that's missing data doesn't overflow the
    // source buffer.
    std::vector<char> scratch;
    scratch.resize(data.size());
    ConstDataRange tooSmallRange(normalBuffer.data(), normalBuffer.size() / 2);
    ASSERT_NOT_OK(
        compressor->decompressData(tooSmallRange, DataRange(scratch.data(), scratch.size())));
}

void checkUndersize(const Message& compressedMsg,
                    std::unique_ptr<MessageCompressorBase> compressor) {
    MessageCompressorRegistry registry;
    const auto compressorName = compressor->getName();

    std::vector<std::string> compressorList = {compressorName};
    registry.setSupportedCompressors(std::move(compressorList));
    registry.registerImplementation(std::move(compressor));
    registry.finalizeSupportedCompressors().transitional_ignore();

    MessageCompressorManager mgr(&registry);
    BSONObjBuilder negotiatorOut;
    std::vector<std::string_view> negotiator({compressorName});
    mgr.serverNegotiate(negotiator, &negotiatorOut);
    checkNegotiationResult(negotiatorOut.done(), {compressorName});

    auto swm = mgr.decompressMessage(compressedMsg);
    ASSERT_EQ(ErrorCodes::BadValue, swm.getStatus());
}

Message buildMessage() {
    const auto data = std::string{"Hello, world!"};
    const auto bufferSize = MsgData::MsgDataHeaderSize + data.size();
    auto buf = SharedBuffer::allocate(bufferSize);
    MsgData::View testView(buf.get());
    testView.setId(123456);
    testView.setResponseToMsgId(654321);
    testView.setOperation(dbQuery);
    testView.setLen(bufferSize);
    memcpy(testView.data(), data.data(), data.size());
    return Message{buf};
}

TEST(MessageCompressorManager, NoCompressionRequested) {
    auto input = BSON("isMaster" << 1);
    checkServerNegotiation(boost::none, {});
}

TEST(MessageCompressorManager, NormalCompressionRequested) {
    std::vector<std::string_view> input{"noop"sv};
    checkServerNegotiation(input, {"noop"});
}

TEST(MessageCompressorManager, BadCompressionRequested) {
    std::vector<std::string_view> input{"fakecompressor"sv};
    checkServerNegotiation(input, {});
}

TEST(MessageCompressorManager, BadAndGoodCompressionRequested) {
    std::vector<std::string_view> input{"fakecompressor"sv, "noop"sv};
    checkServerNegotiation(input, {"noop"});
}

// Transitional: Parse BSON "isMaster"-like docs for compressor lists.
boost::optional<std::vector<std::string_view>> parseBSON(BSONObj input) {
    auto elem = input["compression"];
    if (!elem) {
        return boost::none;
    }

    uassert(ErrorCodes::BadValue,
            str::stream() << "'compression' is not an array: " << elem,
            elem.type() == BSONType::array);

    std::vector<std::string_view> ret;
    for (const auto& e : elem.Obj()) {
        uassert(ErrorCodes::BadValue,
                str::stream() << "'compression' element is not a string: " << e,
                e.type() == BSONType::string);
        ret.push_back(e.valueStringData());
    }

    return ret;
}

TEST(MessageCompressorManager, FullNormalCompression) {
    auto registry = buildRegistry();
    MessageCompressorManager clientManager(&registry);
    MessageCompressorManager serverManager(&registry);

    BSONObjBuilder clientOutput;
    clientManager.clientBegin(&clientOutput);
    auto clientObj = clientOutput.done();
    checkNegotiationResult(clientObj, {"noop"});

    BSONObjBuilder serverOutput;
    serverManager.serverNegotiate(parseBSON(clientObj), &serverOutput);
    auto serverObj = serverOutput.done();
    checkNegotiationResult(serverObj, {"noop"});

    clientManager.clientFinish(serverObj);
}

TEST(NoopMessageCompressor, Fidelity) {
    auto testMessage = buildMessage();
    checkFidelity(testMessage, std::make_unique<NoopMessageCompressor>());
}

TEST(SnappyMessageCompressor, Fidelity) {
    auto testMessage = buildMessage();
    checkFidelity(testMessage, std::make_unique<SnappyMessageCompressor>());
}

TEST(ZlibMessageCompressor, Fidelity) {
    auto testMessage = buildMessage();
    checkFidelity(testMessage, std::make_unique<ZlibMessageCompressor>());
}

TEST(ZstdMessageCompressor, Fidelity) {
    auto testMessage = buildMessage();
    checkFidelity(testMessage, std::make_unique<ZstdMessageCompressor>());
}

TEST(SnappyMessageCompressor, Overflow) {
    checkOverflow(std::make_unique<SnappyMessageCompressor>());
}

TEST(ZlibMessageCompressor, Overflow) {
    checkOverflow(std::make_unique<ZlibMessageCompressor>());
}

TEST(ZstdMessageCompressor, Overflow) {
    checkOverflow(std::make_unique<ZstdMessageCompressor>());
}

TEST(ZlibMessageCompressor, Mismatch) {
    checkOverflow(std::make_unique<ZlibMessageCompressor>());
}

TEST(SnappyMessageCompressor, Undersize) {
    std::vector<std::uint8_t> payload = {
        0x41, 0x0, 0x0,  0x0,  0xad, 0xde, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0xdc,
        0x7,  0x0, 0x0,  0xdd, 0x7,  0x0,  0x0,  0x0,  0x20, 0x0,  0x0,  0x1,  0x27,
        0x0,  0x0, 0x1,  0x1,  0x84, 0xfb, 0x1f, 0x0,  0x0,  0x5,  0x5f, 0x69, 0x64,
        0x0,  0x0, 0x10, 0x0,  0x0,  0x0,  0x48, 0x45, 0x41, 0x50, 0x4c, 0x45, 0x41,
        0x4b, 0x0, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0};


    auto buffer = SharedBuffer::allocate(payload.size());
    std::copy(payload.begin(), payload.end(), buffer.get());

    checkUndersize(Message(buffer), std::make_unique<SnappyMessageCompressor>());
}

TEST(ZlibMessageCompressor, Undersize) {
    std::vector<std::uint8_t> payload = {
        0x3c, 0x00, 0x00, 0x00, 0xad, 0xde, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xdc, 0x07, 0x00,
        0x00, 0xdd, 0x07, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x02, 0x78, 0xda, 0x63, 0x60, 0x00,
        0x82, 0xdf, 0xf2, 0x0c, 0x0c, 0xac, 0xf1, 0x99, 0x29, 0x0c, 0x0c, 0x02, 0x40, 0x9e, 0x87,
        0xab, 0x63, 0x80, 0x8f, 0xab, 0xa3, 0x37, 0x03, 0x12, 0x00, 0x00, 0x6d, 0x26, 0x04, 0x97};

    auto buffer = SharedBuffer::allocate(payload.size());
    std::copy(payload.begin(), payload.end(), buffer.get());

    checkUndersize(Message(buffer), std::make_unique<ZlibMessageCompressor>());
}

TEST(ZstdMessageCompressor, Undersize) {
    std::vector<std::uint8_t> payload = {
        0x44, 0x0,  0x0,  0x0,  0xad, 0xde, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0xdc, 0x7,
        0x0,  0x0,  0xdd, 0x7,  0x0,  0x0,  0x0,  0x20, 0x0,  0x0,  0x3,  0x28, 0xb5, 0x2f,
        0xfd, 0x20, 0x27, 0x15, 0x1,  0x0,  0xe0, 0x0,  0x0,  0x0,  0x0,  0x0,  0xfb, 0x1f,
        0x0,  0x0,  0x5,  0x5f, 0x69, 0x64, 0x0,  0x0,  0x10, 0x0,  0x0,  0x0,  0x48, 0x45,
        0x41, 0x50, 0x4c, 0x45, 0x41, 0x4b, 0x0,  0x1,  0x0,  0x18, 0xc0, 0x9};

    auto buffer = SharedBuffer::allocate(payload.size());
    std::copy(payload.begin(), payload.end(), buffer.get());

    checkUndersize(Message(buffer), std::make_unique<ZstdMessageCompressor>());
}

TEST(MessageCompressorManager, SERVER_28008) {

    // Create a client and server that will negotiate the same compressors,
    // but with a different ordering for the preferred compressor.

    std::unique_ptr<MessageCompressorBase> zstdCompressor =
        std::make_unique<ZstdMessageCompressor>();
    const auto zstdId = zstdCompressor->getId();

    std::unique_ptr<MessageCompressorBase> zlibCompressor =
        std::make_unique<ZlibMessageCompressor>();
    const auto zlibId = zlibCompressor->getId();

    std::unique_ptr<MessageCompressorBase> snappyCompressor =
        std::make_unique<SnappyMessageCompressor>();
    const auto snappyId = snappyCompressor->getId();

    MessageCompressorRegistry registry;
    registry.setSupportedCompressors(
        {snappyCompressor->getName(), zlibCompressor->getName(), zstdCompressor->getName()});
    registry.registerImplementation(std::move(zlibCompressor));
    registry.registerImplementation(std::move(zstdCompressor));
    registry.registerImplementation(std::move(snappyCompressor));
    ASSERT_OK(registry.finalizeSupportedCompressors());

    MessageCompressorManager clientManager(&registry);
    MessageCompressorManager serverManager(&registry);

    // Do negotiation
    BSONObjBuilder clientOutput;
    clientManager.clientBegin(&clientOutput);
    auto clientObj = clientOutput.done();
    BSONObjBuilder serverOutput;
    serverManager.serverNegotiate(parseBSON(clientObj), &serverOutput);
    auto serverObj = serverOutput.done();
    clientManager.clientFinish(serverObj);

    // The preferred compressor is snappy. Check that we round trip as snappy by default.
    auto toSend = buildMessage();
    toSend = assertOk(clientManager.compressMessage(toSend, nullptr));
    MessageCompressorId compressorId;
    auto recvd = assertOk(serverManager.decompressMessage(toSend, &compressorId));
    ASSERT_EQ(compressorId, snappyId);
    toSend = assertOk(serverManager.compressMessage(recvd, nullptr));
    recvd = assertOk(clientManager.decompressMessage(toSend, &compressorId));
    ASSERT_EQ(compressorId, snappyId);

    // Now, force the client to send as zLib. We should round trip as
    // zlib if we feed the out compresor id parameter from
    // decompressMessage back in to compressMessage.
    toSend = buildMessage();
    toSend = assertOk(clientManager.compressMessage(toSend, &zlibId));
    recvd = assertOk(serverManager.decompressMessage(toSend, &compressorId));
    ASSERT_EQ(compressorId, zlibId);
    toSend = assertOk(serverManager.compressMessage(recvd, &compressorId));
    recvd = assertOk(clientManager.decompressMessage(toSend, &compressorId));
    ASSERT_EQ(compressorId, zlibId);

    // Then, force the client to send as zstd. We should round trip as
    // zstd if we feed the out compresor id parameter from
    // decompressMessage back in to compressMessage.
    toSend = buildMessage();
    toSend = assertOk(clientManager.compressMessage(toSend, &zstdId));
    recvd = assertOk(serverManager.decompressMessage(toSend, &compressorId));
    ASSERT_EQ(compressorId, zstdId);
    toSend = assertOk(serverManager.compressMessage(recvd, &compressorId));
    recvd = assertOk(clientManager.decompressMessage(toSend, &compressorId));
    ASSERT_EQ(compressorId, zstdId);
}

TEST(MessageCompressorManager, MessageSizeTooLarge) {
    auto registry = buildRegistry();
    MessageCompressorManager compManager(&registry);

    auto badMessageBuffer = SharedBuffer::allocate(128);
    MsgData::View badMessage(badMessageBuffer.get());
    badMessage.setId(1);
    badMessage.setResponseToMsgId(0);
    badMessage.setOperation(dbCompressed);
    badMessage.setLen(128);

    DataRangeCursor cursor(badMessage.data(), badMessage.data() + badMessage.dataLen());
    cursor.writeAndAdvance<LittleEndian<int32_t>>(dbQuery);
    cursor.writeAndAdvance<LittleEndian<int32_t>>(MaxMessageSizeBytes + 1);
    cursor.writeAndAdvance<LittleEndian<uint8_t>>(registry.getCompressor("noop")->getId());

    auto status = compManager.decompressMessage(Message(badMessageBuffer), nullptr).getStatus();
    ASSERT_NOT_OK(status);
}

TEST(MessageCompressorManager, MessageSizeMax32Bit) {
    auto registry = buildRegistry();
    MessageCompressorManager compManager(&registry);

    auto badMessageBuffer = SharedBuffer::allocate(128);
    MsgData::View badMessage(badMessageBuffer.get());
    badMessage.setId(1);
    badMessage.setResponseToMsgId(0);
    badMessage.setOperation(dbCompressed);
    badMessage.setLen(128);

    DataRangeCursor cursor(badMessage.data(), badMessage.data() + badMessage.dataLen());
    cursor.writeAndAdvance<LittleEndian<int32_t>>(dbQuery);
    cursor.writeAndAdvance<LittleEndian<int32_t>>(std::numeric_limits<int32_t>::max());
    cursor.writeAndAdvance<LittleEndian<uint8_t>>(registry.getCompressor("noop")->getId());

    auto status = compManager.decompressMessage(Message(badMessageBuffer), nullptr).getStatus();
    ASSERT_NOT_OK(status);
}

TEST(MessageCompressorManager, MessageSizeTooSmall) {
    auto registry = buildRegistry();
    MessageCompressorManager compManager(&registry);

    auto badMessageBuffer = SharedBuffer::allocate(128);
    MsgData::View badMessage(badMessageBuffer.get());
    badMessage.setId(1);
    badMessage.setResponseToMsgId(0);
    badMessage.setOperation(dbCompressed);
    badMessage.setLen(128);

    DataRangeCursor cursor(badMessage.data(), badMessage.data() + badMessage.dataLen());
    cursor.writeAndAdvance<LittleEndian<int32_t>>(dbQuery);
    cursor.writeAndAdvance<LittleEndian<int32_t>>(-1);
    cursor.writeAndAdvance<LittleEndian<uint8_t>>(registry.getCompressor("noop")->getId());

    auto status = compManager.decompressMessage(Message(badMessageBuffer), nullptr).getStatus();
    ASSERT_NOT_OK(status);
}

TEST(MessageCompressorManager, RuntMessage) {
    auto registry = buildRegistry();
    MessageCompressorManager compManager(&registry);

    auto badMessageBuffer = SharedBuffer::allocate(128);
    MsgData::View badMessage(badMessageBuffer.get());
    badMessage.setId(1);
    badMessage.setResponseToMsgId(0);
    badMessage.setOperation(dbCompressed);
    badMessage.setLen(MsgData::MsgDataHeaderSize + 8);

    // This is a totally bogus compression header of just the original opcode + 0 byte uncompressed
    // size
    DataRangeCursor cursor(badMessage.data(), badMessage.data() + badMessage.dataLen());
    cursor.writeAndAdvance<LittleEndian<int32_t>>(dbQuery);
    cursor.writeAndAdvance<LittleEndian<int32_t>>(0);

    auto status = compManager.decompressMessage(Message(badMessageBuffer), nullptr).getStatus();
    ASSERT_NOT_OK(status);
}

class ZlibDecompressTest : public unittest::Test {
public:
    Status doDecompress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
        ConstDataRange inRange(in.data(), in.size());
        DataRange outRange(out.data(), out.size());
        auto swSize = compressor->decompressData(inRange, outRange);
        if (swSize.isOK())
            out.resize(swSize.getValue());
        return swSize.getStatus();
    }

    Status doDecompress(const std::vector<uint8_t>& in) {
        std::vector<uint8_t> out(1024);
        return doDecompress(in, out);
    }

    std::vector<uint8_t> doCompress(const std::vector<uint8_t>& in) {
        std::vector<uint8_t> out(compressor->getMaxCompressedSize(in.size()));
        DataRange outRange(out.data(), out.size());
        auto swSz = compressor->compressData(in, outRange);
        ASSERT_OK(swSz);
        out.resize(swSz.getValue());
        return out;
    }

    static std::vector<uint8_t> strVec(std::string_view s) {
        return std::vector<uint8_t>(s.begin(), s.end());
    }

    std::unique_ptr<ZlibMessageCompressor> compressor{std::make_unique<ZlibMessageCompressor>()};
};

TEST_F(ZlibDecompressTest, RejectsEmptyPayload) {
    ASSERT_EQ(doDecompress({}), ErrorCodes::BadValue);
}

TEST_F(ZlibDecompressTest, RejectsUndersizedPayload) {
    ASSERT_EQ(doDecompress({0x78, 0x9c, 0x03, 0x00}), ErrorCodes::BadValue);
}

TEST_F(ZlibDecompressTest, RejectsBadCompressionMethod) {
    const uint8_t cm = 0;  // Expected to be 8.
    const uint8_t cmf = 0x70 | (cm & 0xf);
    ASSERT_EQ(doDecompress({cmf, 0x9c, 0x63, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00}),
              ErrorCodes::BadValue);
}

TEST_F(ZlibDecompressTest, RoundTripLongerData) {
    const auto data = strVec("Hello, MongoDB! Compression hardening test.");
    std::vector<uint8_t> out(data.size() + 100);
    ASSERT_OK(doDecompress(doCompress(data), out));
    ASSERT_EQ(out, data);
}

TEST_F(ZlibDecompressTest, RoundTripShortData) {
    const auto data = strVec("Short");
    std::vector<uint8_t> out(10000);
    ASSERT_OK(doDecompress(doCompress(data), out));
    ASSERT_EQ(out, data);
}

TEST_F(ZlibDecompressTest, ByteCounts) {
    const auto data = strVec("Hello, MongoDB! Compression hardening test.");
    uint64_t compressIn = 0;
    uint64_t compressOut = 0;
    uint64_t decompressIn = 0;
    uint64_t decompressOut = 0;
    for (int i = 0;; ++i) {
        ASSERT_EQ(compressor->getCompressorBytesIn(), compressIn);
        ASSERT_EQ(compressor->getCompressorBytesOut(), compressOut);
        ASSERT_EQ(compressor->getDecompressorBytesIn(), decompressIn);
        ASSERT_EQ(compressor->getDecompressorBytesOut(), decompressOut);
        if (i >= 5)
            break;

        std::vector<uint8_t> compressed = doCompress(data);
        compressIn += data.size();
        compressOut += compressed.size();

        std::vector<uint8_t> out(data.size() + 100);
        ASSERT_OK(doDecompress(compressed, out));
        ASSERT_EQ(out, data);
        decompressIn += compressed.size();
        decompressOut += data.size();
    }
}

void checkWrongUncompressedSize(std::unique_ptr<MessageCompressorBase> compressor,
                                std::string expectedError) {
    MessageCompressorRegistry registry;
    const auto compressorId = compressor->getId();

    std::vector<std::string> compressorList = {compressor->getName()};
    registry.setSupportedCompressors(std::move(compressorList));
    registry.registerImplementation(std::move(compressor));
    ASSERT_OK(registry.finalizeSupportedCompressors());
    MessageCompressorManager manager(&registry);

    OpMsgBuilder msgBuilder;
    msgBuilder.setBody(BSON("ping" << 1));
    Message originalMsg = msgBuilder.finishWithoutSizeChecking();
    const auto originalView = originalMsg.singleData();
    const size_t originalDataSize = originalView.dataLen();

    auto swCompressed = manager.compressMessage(originalMsg, &compressorId);
    ASSERT_OK(swCompressed);
    Message properlyCompressedMsg = std::move(swCompressed.getValue());

    const auto compressedView = properlyCompressedMsg.singleData();
    ConstDataRangeCursor input(compressedView.data(),
                               compressedView.data() + compressedView.dataLen());

    CompressionHeader originalHeader(&input);

    const size_t compressedDataSize = input.length();
    const char* compressedDataPtr = input.data();

    ASSERT_EQ(originalHeader.compressorId, compressorId);
    ASSERT_EQ(originalHeader.originalOpCode, dbMsg);

    const int32_t wrongUncompressedSize = static_cast<int32_t>(originalDataSize * 2);

    const size_t malformedMessageSize =
        MsgData::MsgDataHeaderSize + CompressionHeader::size() + compressedDataSize;
    auto malformedBuffer = SharedBuffer::allocate(malformedMessageSize);

    MsgData::View malformedView(malformedBuffer.get());
    malformedView.setId(originalView.getId());
    malformedView.setResponseToMsgId(originalView.getResponseToMsgId());
    malformedView.setOperation(dbCompressed);
    malformedView.setLen(malformedMessageSize);

    DataRangeCursor output(malformedView.data(), malformedView.data() + malformedView.dataLen());
    output.writeAndAdvance<LittleEndian<int32_t>>(originalHeader.originalOpCode);
    output.writeAndAdvance<LittleEndian<int32_t>>(wrongUncompressedSize);
    output.writeAndAdvance<LittleEndian<uint8_t>>(originalHeader.compressorId);

    std::memcpy(output.data(), compressedDataPtr, compressedDataSize);

    Message malformedMessage(malformedBuffer);

    auto swDecompressed = manager.decompressMessage(malformedMessage);
    ASSERT_NOT_OK(swDecompressed.getStatus());
    ASSERT_EQ(swDecompressed.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_STRING_CONTAINS(swDecompressed.getStatus().reason(), expectedError);
}

TEST(SnappyMessageCompressor, WrongUncompressedSize) {
    checkWrongUncompressedSize(std::make_unique<SnappyMessageCompressor>(),
                               "Uncompressed message size does not match expected size");
}

TEST(ZstdMessageCompressor, WrongUncompressedSize) {
    checkWrongUncompressedSize(std::make_unique<ZstdMessageCompressor>(),
                               "Uncompressed message size does not match expected size");
}

TEST(ZlibMessageCompressor, WrongUncompressedSize) {
    checkWrongUncompressedSize(std::make_unique<ZlibMessageCompressor>(),
                               "Decompressing message returned less data than expected");
}

}  // namespace
}  // namespace mongo
