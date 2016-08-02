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
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/message_compressor_noop.h"
#include "mongo/unittest/unittest.h"

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
    ret.finalizeSupportedCompressors();

    return std::move(ret);
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
}  // namespace

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
}  // namespace mongo
