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
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
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
    MessageCompressorManager manager(MessageCompressionDirection::kIngress, &registry);

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

    MessageCompressorManager mgr(MessageCompressionDirection::kIngress, &registry);
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

    MessageCompressorManager mgr(MessageCompressionDirection::kIngress, &registry);
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
    MessageCompressorManager clientManager(MessageCompressionDirection::kEgress, &registry);
    MessageCompressorManager serverManager(MessageCompressionDirection::kIngress, &registry);

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

    MessageCompressorManager clientManager(MessageCompressionDirection::kEgress, &registry);
    MessageCompressorManager serverManager(MessageCompressionDirection::kIngress, &registry);

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
    MessageCompressorManager compManager(MessageCompressionDirection::kIngress, &registry);

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
    MessageCompressorManager compManager(MessageCompressionDirection::kIngress, &registry);

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
    MessageCompressorManager compManager(MessageCompressionDirection::kIngress, &registry);

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
    MessageCompressorManager compManager(MessageCompressionDirection::kIngress, &registry);

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
    void SetUp() override {
        auto owned = std::make_unique<ZlibMessageCompressor>();
        compressor = owned.get();
        registry.setSupportedCompressors({compressor->getName()});
        registry.registerImplementation(std::move(owned));
        ASSERT_OK(registry.finalizeSupportedCompressors());
    }

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

    MessageCompressorRegistry registry;
    ZlibMessageCompressor* compressor = nullptr;  // Owned by registry after SetUp.
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

TEST_F(ZlibDecompressTest, DirectCallsCountByDirection) {
    const auto data = strVec("Hello, MongoDB!");

    std::vector<uint8_t> compressed(compressor->getMaxCompressedSize(data.size()));
    auto swCompressed = compressor->compressData(data,
                                                 DataRange(compressed.data(), compressed.size()),
                                                 MessageCompressionDirection::kEgress);
    ASSERT_OK(swCompressed);
    compressed.resize(swCompressed.getValue());

    ASSERT_EQ(compressor->getCompressorBytesIn(MessageCompressionDirection::kEgress), data.size());
    ASSERT_EQ(compressor->getCompressorBytesOut(MessageCompressionDirection::kEgress),
              compressed.size());
    ASSERT_EQ(compressor->getCompressorBytesIn(MessageCompressionDirection::kIngress), 0);
    ASSERT_EQ(compressor->getCompressorBytesOut(MessageCompressionDirection::kIngress), 0);
    ASSERT_EQ(compressor->getCompressorBytesIn(), data.size());

    std::vector<uint8_t> out(1024);
    auto swOut = compressor->decompressData(
        compressed, DataRange(out.data(), out.size()), MessageCompressionDirection::kIngress);
    ASSERT_OK(swOut);

    ASSERT_EQ(compressor->getDecompressorBytesIn(MessageCompressionDirection::kIngress),
              compressed.size());
    ASSERT_EQ(compressor->getDecompressorBytesOut(MessageCompressionDirection::kIngress),
              data.size());
    ASSERT_EQ(compressor->getDecompressorBytesIn(MessageCompressionDirection::kEgress), 0);
    ASSERT_EQ(compressor->getDecompressorBytesOut(MessageCompressionDirection::kEgress), 0);

    // A failed call must not move any counter: sum over both directions before and after.
    const auto compressorInBefore = compressor->getCompressorBytesIn();
    const auto compressorOutBefore = compressor->getCompressorBytesOut();
    const auto decompressorInBefore = compressor->getDecompressorBytesIn();
    const auto decompressorOutBefore = compressor->getDecompressorBytesOut();

    std::array<uint8_t, 2> tooSmall;
    ASSERT_NOT_OK(compressor->compressData(
        data, DataRange(tooSmall.data(), tooSmall.size()), MessageCompressionDirection::kEgress));

    ConstDataRange truncated(compressed.data(), compressed.size() / 2);
    std::vector<uint8_t> scratch(1024);
    ASSERT_NOT_OK(compressor->decompressData(truncated,
                                             DataRange(scratch.data(), scratch.size()),
                                             MessageCompressionDirection::kEgress));

    ASSERT_EQ(compressor->getCompressorBytesIn(), compressorInBefore);
    ASSERT_EQ(compressor->getCompressorBytesOut(), compressorOutBefore);
    ASSERT_EQ(compressor->getDecompressorBytesIn(), decompressorInBefore);
    ASSERT_EQ(compressor->getDecompressorBytesOut(), decompressorOutBefore);
}

TEST_F(ZlibDecompressTest, ByteCounts) {
    MessageCompressorManager manager(MessageCompressionDirection::kIngress, &registry);
    const auto compressorId = compressor->getId();

    OpMsgBuilder msgBuilder;
    msgBuilder.setBody(BSON("ping" << 1));
    Message originalMsg = msgBuilder.finishWithoutSizeChecking();
    const size_t dataLen = originalMsg.header().dataLen();

    for (int i = 0;; ++i) {
        ASSERT_EQ(compressor->getCompressorBytesIn(), dataLen * i);
        ASSERT_EQ(compressor->getDecompressorBytesOut(), dataLen * i);
        ASSERT_EQ(compressor->getCompressorBytesOut(), compressor->getDecompressorBytesIn());
        if (i >= 5)
            break;

        auto swCompressed = manager.compressMessage(originalMsg, &compressorId);
        ASSERT_OK(swCompressed);
        ASSERT_EQ(compressor->getCompressorBytesIn(MessageCompressionDirection::kIngress),
                  dataLen * (i + 1));
        ASSERT_EQ(compressor->getCompressorBytesIn(MessageCompressionDirection::kEgress), 0);
        const Message compressedMsg = std::move(swCompressed.getValue());

        auto swDecompressed = manager.decompressMessage(compressedMsg);
        ASSERT_OK(swDecompressed);
        ASSERT_EQ(compressor->getDecompressorBytesOut(MessageCompressionDirection::kIngress),
                  dataLen * (i + 1));
        ASSERT_EQ(compressor->getDecompressorBytesOut(MessageCompressionDirection::kEgress), 0);
    }
}

TEST_F(ZlibDecompressTest, UncountedManagerDoesNotRecordBytes) {
    MessageCompressorManager manager(MessageCompressionDirection::kUncounted, &registry);
    const auto compressorId = compressor->getId();

    OpMsgBuilder msgBuilder;
    msgBuilder.setBody(BSON("ping" << 1));
    Message originalMsg = msgBuilder.finishWithoutSizeChecking();

    auto swCompressed = manager.compressMessage(originalMsg, &compressorId);
    ASSERT_OK(swCompressed);
    ASSERT_EQ(compressor->getCompressorBytesIn(), 0);
    ASSERT_EQ(compressor->getCompressorBytesOut(), 0);

    auto swDecompressed = manager.decompressMessage(swCompressed.getValue());
    ASSERT_OK(swDecompressed);
    ASSERT_EQ(compressor->getDecompressorBytesIn(), 0);
    ASSERT_EQ(compressor->getDecompressorBytesOut(), 0);
}

TEST_F(ZlibDecompressTest, IngressCountsSumAcrossManagers) {
    MessageCompressorManager ingressManager1(MessageCompressionDirection::kIngress, &registry);
    MessageCompressorManager ingressManager2(MessageCompressionDirection::kIngress, &registry);
    MessageCompressorManager egressManager(MessageCompressionDirection::kEgress, &registry);
    const auto compressorId = compressor->getId();

    OpMsgBuilder msgBuilder;
    msgBuilder.setBody(BSON("ping" << 1));
    Message originalMsg = msgBuilder.finishWithoutSizeChecking();
    const size_t dataLen = originalMsg.header().dataLen();

    auto swIngressCompressed1 = ingressManager1.compressMessage(originalMsg, &compressorId);
    ASSERT_OK(swIngressCompressed1);
    auto swIngressCompressed2 = ingressManager2.compressMessage(originalMsg, &compressorId);
    ASSERT_OK(swIngressCompressed2);
    auto swEgressCompressed = egressManager.compressMessage(originalMsg, &compressorId);
    ASSERT_OK(swEgressCompressed);

    // Two ingress managers recorded twice as much as the one egress manager.
    ASSERT_EQ(compressor->getCompressorBytesIn(MessageCompressionDirection::kIngress), dataLen * 2);
    ASSERT_EQ(compressor->getCompressorBytesIn(MessageCompressionDirection::kEgress), dataLen);
    ASSERT_EQ(compressor->getCompressorBytesIn(MessageCompressionDirection::kIngress),
              2 * compressor->getCompressorBytesIn(MessageCompressionDirection::kEgress));
    ASSERT_EQ(compressor->getCompressorBytesIn(), dataLen * 3);
}

TEST_F(ZlibDecompressTest, ManagerBumpsOtelNetworkCompressionCounters) {
    // Create before recording: on OTel platforms collection starts empty, so the absolute
    // values read below contain only this test's bumps.
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        GTEST_SKIP() << "OTel not available";
    }

    MessageCompressorManager manager(MessageCompressionDirection::kIngress, &registry);
    const auto compressorId = compressor->getId();

    OpMsgBuilder msgBuilder;
    msgBuilder.setBody(BSON("ping" << 1));
    Message originalMsg = msgBuilder.finishWithoutSizeChecking();
    const size_t dataLen = originalMsg.header().dataLen();

    auto swCompressed = manager.compressMessage(originalMsg, &compressorId);
    ASSERT_OK(swCompressed);
    auto swDecompressed = manager.decompressMessage(swCompressed.getValue());
    ASSERT_OK(swDecompressed);

    const auto zlibBytes = std::make_tuple(std::string_view{"zlib"});
    ASSERT_EQ(
        capturer.readInt64Counter(
            otel::metrics::MetricNames::kNetworkCompressionIngressCompressorBytesIn, zlibBytes),
        dataLen);
    ASSERT_EQ(
        capturer.readInt64Counter(
            otel::metrics::MetricNames::kNetworkCompressionIngressDecompressorBytesOut, zlibBytes),
        dataLen);
    // The compressed size isn't fixed, but both sides of the wire must agree.
    ASSERT_EQ(
        capturer.readInt64Counter(
            otel::metrics::MetricNames::kNetworkCompressionIngressCompressorBytesOut, zlibBytes),
        capturer.readInt64Counter(
            otel::metrics::MetricNames::kNetworkCompressionIngressDecompressorBytesIn, zlibBytes));
}

void checkWrongUncompressedSize(std::unique_ptr<MessageCompressorBase> compressor,
                                std::string expectedError) {
    MessageCompressorRegistry registry;
    const auto compressorId = compressor->getId();

    std::vector<std::string> compressorList = {compressor->getName()};
    registry.setSupportedCompressors(std::move(compressorList));
    registry.registerImplementation(std::move(compressor));
    ASSERT_OK(registry.finalizeSupportedCompressors());
    MessageCompressorManager manager(MessageCompressionDirection::kIngress, &registry);

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

Message makeSnappyPrefixMessage(const std::vector<uint8_t>& raw,
                                int32_t uncompressedSize = 4,
                                int32_t originalOpCode = dbMsg,
                                MessageCompressorId compressorId =
                                    static_cast<MessageCompressorId>(MessageCompressor::kSnappy)) {
    const auto size = MsgData::MsgDataHeaderSize + CompressionHeader::size() + raw.size();
    auto buffer = SharedBuffer::allocate(size);
    MsgData::View header(buffer.get());
    header.setLen(size);
    header.setId(123);
    header.setResponseToMsgId(456);
    header.setOperation(dbCompressed);
    DataRangeCursor output(header.data(), header.data() + header.dataLen());
    CompressionHeader compressionHeader(originalOpCode, uncompressedSize, compressorId);
    compressionHeader.serialize(&output);
    if (!raw.empty()) {
        std::memcpy(output.data(), raw.data(), raw.size());
    }
    return Message(std::move(buffer));
}

TEST(SnappyOpMsgFlags, MatchesFullDecompressionWithoutUpdatingCounters) {
    MessageCompressorRegistry registry;
    auto compressor = std::make_unique<SnappyMessageCompressor>();
    auto* counters = compressor.get();
    const auto compressorId = compressor->getId();
    registry.setSupportedCompressors({compressor->getName()});
    registry.registerImplementation(std::move(compressor));
    ASSERT_OK(registry.finalizeSupportedCompressors());
    MessageCompressorManager manager(&registry);

    for (uint32_t flags : {0u,
                           OpMsg::kMoreToCome,
                           OpMsg::kExhaustSupported | OpMsg::kMoreToCome,
                           OpMsg::kChecksumPresent}) {
        OpMsgBuilder builder;
        builder.setBody(BSON("ok" << 1 << "payload" << std::string(1024, 'x')));
        auto original = builder.finish();
        OpMsg::replaceFlags(&original, flags & ~OpMsg::kChecksumPresent);
        if (flags & OpMsg::kChecksumPresent) {
            OpMsg::appendChecksum(&original);
        }
        auto compressed = assertOk(manager.compressMessage(original, &compressorId));
        const auto bytesIn = counters->getDecompressorBytesIn();
        const auto bytesOut = counters->getDecompressorBytesOut();
        // appendChecksum is a no-op in builds without WiredTiger checksum support.
        const auto expectedFlags = OpMsg::flags(original);
        ASSERT_EQ(assertOk(readSnappyOpMsgFlags(compressed)), expectedFlags);
        ASSERT_EQ(counters->getDecompressorBytesIn(), bytesIn);
        ASSERT_EQ(counters->getDecompressorBytesOut(), bytesOut);
        auto decompressed = assertOk(manager.decompressMessage(compressed));
        ASSERT_EQ(OpMsg::flags(decompressed), expectedFlags);
        ASSERT_EQ(OpMsg::parse(decompressed).body.woCompare(OpMsg::parse(original).body), 0);
    }
}

TEST(SnappyOpMsgFlags, MultipleShortLiteralsAndOptionalFlags) {
    const auto message = makeSnappyPrefixMessage({4, 0, 3, 0, 0, 0, 1, 0, 0x80});
    ASSERT_EQ(assertOk(readSnappyOpMsgFlags(message)),
              0x80000000u | OpMsg::kExhaustSupported | OpMsg::kChecksumPresent |
                  OpMsg::kMoreToCome);
}

TEST(SnappyOpMsgFlags, AllLongLiteralLengthEncodings) {
    for (uint8_t lengthBytes = 1; lengthBytes <= 4; ++lengthBytes) {
        std::vector<uint8_t> raw{60, static_cast<uint8_t>((59 + lengthBytes) << 2), 59};
        raw.insert(raw.end(), lengthBytes - 1, 0);
        raw.insert(raw.end(), {3, 0, 1, 0});
        raw.insert(raw.end(), 56, 0);
        ASSERT_EQ(assertOk(readSnappyOpMsgFlags(makeSnappyPrefixMessage(raw, 60))),
                  OpMsg::kChecksumPresent | OpMsg::kMoreToCome | OpMsg::kExhaustSupported);
    }
}

TEST(SnappyOpMsgFlags, CopyOneWithOverlap) {
    ASSERT_EQ(assertOk(readSnappyOpMsgFlags(makeSnappyPrefixMessage({5, 0, 0, 1, 1}, 5))), 0u);
    ASSERT_EQ(assertOk(readSnappyOpMsgFlags(makeSnappyPrefixMessage({12, 0, 0, 29, 1}, 12))), 0u);
}

TEST(SnappyOpMsgFlags, CopyTwoAndFourWithOverlap) {
    ASSERT_EQ(assertOk(readSnappyOpMsgFlags(makeSnappyPrefixMessage({4, 0, 0, 10, 1, 0}))), 0u);
    ASSERT_EQ(assertOk(readSnappyOpMsgFlags(makeSnappyPrefixMessage({4, 0, 0, 11, 1, 0, 0, 0}))),
              0u);
    ASSERT_EQ(assertOk(readSnappyOpMsgFlags(makeSnappyPrefixMessage({4, 0, 0, 0, 0, 6, 2, 0}))),
              0u);
    ASSERT_EQ(assertOk(readSnappyOpMsgFlags(makeSnappyPrefixMessage({4, 4, 3, 0, 6, 2, 0}))),
              0x00030003u);
    ASSERT_EQ(assertOk(readSnappyOpMsgFlags(
                  makeSnappyPrefixMessage({4, 0, 0, 2, 1, 0, 3, 2, 0, 0, 0, 2, 3, 0}))),
              0u);
}

TEST(SnappyOpMsgFlags, RejectsTruncatedOrOverflowingVarints) {
    for (const std::vector<uint8_t>& raw :
         std::vector<std::vector<uint8_t>>{{},
                                           {0x80},
                                           {0x80, 0x80},
                                           {0x80, 0x80, 0x80},
                                           {0x80, 0x80, 0x80, 0x80},
                                           {0x84, 0x80, 0x80, 0x80, 0x10},
                                           {0x84, 0x80, 0x80, 0x80, 0x80, 0},
                                           {0xff, 0xff, 0xff, 0xff, 0x0f}}) {
        ASSERT_EQ(readSnappyOpMsgFlags(makeSnappyPrefixMessage(raw)).getStatus().code(),
                  ErrorCodes::BadValue);
    }
    ASSERT_EQ(assertOk(readSnappyOpMsgFlags(
                  makeSnappyPrefixMessage({0x84, 0x80, 0x80, 0x80, 0, 12, 0, 0, 0, 0}))),
              0u);
}

TEST(SnappyOpMsgFlags, RejectsMismatchedOrInvalidDeclaredSize) {
    const std::vector<uint8_t> raw{4, 12, 0, 0, 0, 0};
    for (int32_t size :
         {-1,
          0,
          1,
          2,
          3,
          5,
          std::numeric_limits<int32_t>::max(),
          static_cast<int32_t>(MaxMessageSizeBytes - MsgData::MsgDataHeaderSize + 1)}) {
        ASSERT_EQ(readSnappyOpMsgFlags(makeSnappyPrefixMessage(raw, size)).getStatus().code(),
                  ErrorCodes::BadValue);
    }
    const auto maxSize = static_cast<int32_t>(MaxMessageSizeBytes - MsgData::MsgDataHeaderSize);
    for (int32_t size : {4, 128, 16384, maxSize}) {
        std::vector<uint8_t> prefix;
        for (uint32_t remaining = size;; remaining >>= 7) {
            const auto byte = static_cast<uint8_t>(remaining & 0x7f);
            prefix.push_back(byte | (remaining > 0x7f ? 0x80 : 0));
            if (remaining <= 0x7f) {
                break;
            }
        }
        prefix.insert(prefix.end(), {12, 0, 0, 0, 0});
        ASSERT_EQ(assertOk(readSnappyOpMsgFlags(makeSnappyPrefixMessage(prefix, size))), 0u);
    }
}

TEST(SnappyOpMsgFlags, RejectsTruncatedShortLiterals) {
    for (const std::vector<uint8_t>& raw : std::vector<std::vector<uint8_t>>{
             {4}, {4, 12}, {4, 12, 0}, {4, 12, 0, 0}, {4, 12, 0, 0, 0}, {4, 0, 0}}) {
        ASSERT_EQ(readSnappyOpMsgFlags(makeSnappyPrefixMessage(raw)).getStatus().code(),
                  ErrorCodes::BadValue);
    }
    ASSERT_EQ(
        readSnappyOpMsgFlags(makeSnappyPrefixMessage({8, 28, 0, 0, 0, 0}, 8)).getStatus().code(),
        ErrorCodes::BadValue);
}

TEST(SnappyOpMsgFlags, RejectsTruncatedLongLiteralLengthsAndPayloads) {
    for (uint8_t lengthBytes = 1; lengthBytes <= 4; ++lengthBytes) {
        const auto tag = static_cast<uint8_t>((59 + lengthBytes) << 2);
        for (uint8_t available = 0; available < lengthBytes; ++available) {
            std::vector<uint8_t> raw{4, tag};
            raw.insert(raw.end(), available, 0);
            ASSERT_EQ(readSnappyOpMsgFlags(makeSnappyPrefixMessage(raw)).getStatus().code(),
                      ErrorCodes::BadValue);
        }
        std::vector<uint8_t> raw{4, tag, 3};
        raw.insert(raw.end(), lengthBytes - 1, 0);
        raw.insert(raw.end(), 3, 0);
        ASSERT_EQ(readSnappyOpMsgFlags(makeSnappyPrefixMessage(raw)).getStatus().code(),
                  ErrorCodes::BadValue);
    }
}

TEST(SnappyOpMsgFlags, RejectsLiteralLengthOverflowAndOutputOverrun) {
    ASSERT_EQ(readSnappyOpMsgFlags(makeSnappyPrefixMessage({4, 252, 255, 255, 255, 255}))
                  .getStatus()
                  .code(),
              ErrorCodes::BadValue);
    ASSERT_EQ(
        readSnappyOpMsgFlags(makeSnappyPrefixMessage({4, 16, 0, 0, 0, 0, 0})).getStatus().code(),
        ErrorCodes::BadValue);
}

TEST(SnappyOpMsgFlags, RejectsTruncatedCopyOffsets) {
    for (const auto [tag, offsetBytes] :
         std::array<std::pair<uint8_t, size_t>, 3>{{{1, 1}, {10, 2}, {11, 4}}}) {
        for (size_t available = 0; available < offsetBytes; ++available) {
            std::vector<uint8_t> raw{5, 0, 0, tag};
            raw.insert(raw.end(), available, 1);
            ASSERT_EQ(readSnappyOpMsgFlags(makeSnappyPrefixMessage(raw, 5)).getStatus().code(),
                      ErrorCodes::BadValue);
        }
    }
}

TEST(SnappyOpMsgFlags, RejectsZeroOrTooDistantCopyOffsets) {
    for (const std::vector<uint8_t>& raw :
         std::vector<std::vector<uint8_t>>{{5, 0, 0, 1, 0},
                                           {5, 0, 0, 1, 2},
                                           {5, 0, 0, 33, 1},
                                           {5, 0, 0, 10, 0, 0},
                                           {5, 0, 0, 10, 2, 0},
                                           {5, 0, 0, 11, 0, 0, 0, 0},
                                           {5, 0, 0, 11, 2, 0, 0, 0},
                                           {5, 0, 0, 11, 255, 255, 255, 255},
                                           {5, 1, 1},
                                           {5, 10, 1, 0},
                                           {5, 11, 1, 0, 0, 0}}) {
        ASSERT_EQ(readSnappyOpMsgFlags(makeSnappyPrefixMessage(raw, 5)).getStatus().code(),
                  ErrorCodes::BadValue);
    }
}

TEST(SnappyOpMsgFlags, RejectsCopyOutputOverrun) {
    for (const std::vector<uint8_t>& raw : std::vector<std::vector<uint8_t>>{
             {4, 0, 0, 1, 1}, {4, 0, 0, 14, 1, 0}, {4, 0, 0, 15, 1, 0, 0, 0}}) {
        ASSERT_EQ(readSnappyOpMsgFlags(makeSnappyPrefixMessage(raw)).getStatus().code(),
                  ErrorCodes::BadValue);
    }
}

TEST(SnappyOpMsgFlags, RejectsUnknownRequiredFlags) {
    for (uint32_t bit = 2; bit < 16; ++bit) {
        std::vector<uint8_t> raw{4, 12, 0, 0, 0, 0};
        raw[2 + bit / 8] = static_cast<uint8_t>(1u << (bit % 8));
        ASSERT_EQ(readSnappyOpMsgFlags(makeSnappyPrefixMessage(raw)).getStatus().code(),
                  ErrorCodes::IllegalOpMsgFlag);
    }
}

TEST(SnappyOpMsgFlags, RejectsInvalidOuterHeadersAndCompressionKinds) {
    ASSERT_EQ(readSnappyOpMsgFlags(Message{}).getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(readSnappyOpMsgFlags(Message(SharedBuffer::allocate(3))).getStatus().code(),
              ErrorCodes::BadValue);
    const std::vector<uint8_t> raw{4, 12, 0, 0, 0, 0};
    auto message = makeSnappyPrefixMessage(raw);
    for (int32_t size = 0;
         size < MsgData::MsgDataHeaderSize + static_cast<int32_t>(CompressionHeader::size());
         ++size) {
        message.header().setLen(size);
        ASSERT_EQ(readSnappyOpMsgFlags(message).getStatus().code(), ErrorCodes::BadValue);
    }
    for (int32_t size : {-1,
                         static_cast<int32_t>(message.capacity() + 1),
                         static_cast<int32_t>(MaxMessageSizeBytes + 1)}) {
        message.header().setLen(size);
        ASSERT_EQ(readSnappyOpMsgFlags(message).getStatus().code(), ErrorCodes::BadValue);
    }
    message = makeSnappyPrefixMessage(raw);
    message.header().setOperation(dbMsg);
    ASSERT_EQ(readSnappyOpMsgFlags(message).getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(readSnappyOpMsgFlags(makeSnappyPrefixMessage(raw, 4, dbQuery)).getStatus().code(),
              ErrorCodes::BadValue);
    for (auto compressorId : {MessageCompressor::kNoop,
                              MessageCompressor::kZlib,
                              MessageCompressor::kZstd,
                              MessageCompressor::kExtended}) {
        ASSERT_EQ(
            readSnappyOpMsgFlags(makeSnappyPrefixMessage(
                                     raw, 4, dbMsg, static_cast<MessageCompressorId>(compressorId)))
                .getStatus()
                .code(),
            ErrorCodes::BadValue);
    }
}

TEST(SnappyOpMsgFlags, SuccessfulPrefixDoesNotValidateCorruptTail) {
    for (const std::vector<uint8_t>& raw : std::vector<std::vector<uint8_t>>{
             {8, 12, 0, 0, 0, 0}, {8, 12, 0, 0, 0, 0, 1, 0}, {8, 12, 0, 0, 0, 0, 252}}) {
        auto message = makeSnappyPrefixMessage(raw, 8);
        ASSERT_EQ(assertOk(readSnappyOpMsgFlags(message)), 0u);
        std::array<char, 8> output{};
        SnappyMessageCompressor compressor;
        ASSERT_NOT_OK(compressor.decompressData(ConstDataRange(raw.data(), raw.size()),
                                                DataRange(output.data(), output.size())));
    }
}

}  // namespace
}  // namespace mongo
