// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/op_msg_fuzzer_shard_fixture.h"

#include "mongo/bson/json.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

TEST(OpMsgFuzzerShardFixtureTest, Ping) {
    auto fixture = OpMsgFuzzerShardFixture(/* skipGlobalInitializers */ true);

    auto msg = [] {
        OpMsgBuilder msgBuilder;

        msgBuilder.setBody(fromjson(R"({ ping: 1 })"));

        return msgBuilder.finishWithoutSizeChecking();
    }();
    ASSERT_EQ(fixture.testOneInput(msg.buf(), msg.size()), 0);
}

}  // namespace
}  // namespace mongo
