/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/message_compressor_noop.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/transport/message_compressor_snappy.h"
#include "mongo/transport/message_compressor_zlib.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/message.h"

#include <string>
#include <vector>

namespace mongo {
namespace {
MessageCompressorRegistry buildRegistry() {
    MessageCompressorRegistry ret;
    auto compressor = stdx::make_unique<NoopMessageCompressor>();

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
        resultAlgos.push_back(e.checkAndGetStringData().toString());
    }
    ASSERT_EQ(algos.size(), resultAlgos.size());
    for (size_t i = 0; i < algos.size(); i++) {
        ASSERT_EQ(algos[i], resultAlgos[i]);
    }
}

void checkServerNegotiation(const BSONObj& input, const std::vector<std::string>& expected) {
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
    auto negotiator = BSON("isMaster" << 1 << "compression" << BSON_ARRAY(compressorName));
    BSONObjBuilder negotiatorOut;
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
    checkServerNegotiation(input, {});
}

TEST(MessageCompressorManager, NormalCompressionRequested) {
    auto input = BSON("isMaster" << 1 << "compression" << BSON_ARRAY("noop"));
    checkServerNegotiation(input, {"noop"});
}

TEST(MessageCompressorManager, BadCompressionRequested) {
    auto input = BSON("isMaster" << 1 << "compression" << BSON_ARRAY("fakecompressor"));
    checkServerNegotiation(input, {});
}

TEST(MessageCompressorManager, BadAndGoodCompressionRequested) {
    auto input = BSON("isMaster" << 1 << "compression" << BSON_ARRAY("fakecompressor"
                                                                     << "noop"));
    checkServerNegotiation(input, {"noop"});
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
    serverManager.serverNegotiate(clientObj, &serverOutput);
    auto serverObj = serverOutput.done();
    checkNegotiationResult(serverObj, {"noop"});

    clientManager.clientFinish(serverObj);
}

TEST(NoopMessageCompressor, Fidelity) {
    auto testMessage = buildMessage();
    checkFidelity(testMessage, stdx::make_unique<NoopMessageCompressor>());
}

TEST(SnappyMessageCompressor, Fidelity) {
    auto testMessage = buildMessage();
    checkFidelity(testMessage, stdx::make_unique<SnappyMessageCompressor>());
}

TEST(ZlibMessageCompressor, Fidelity) {
    auto testMessage = buildMessage();
    checkFidelity(testMessage, stdx::make_unique<ZlibMessageCompressor>());
}

}  // namespace mongo
}  // namespace
