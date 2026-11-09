// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/op_msg_fuzzer_shard_fixture.h"

#include "mongo/bson/json.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

/**
 * Returns the result of fixture.testOneInput on a message with the provided body.
 */
int runCommand(OpMsgFuzzerShardFixture& fixture, const BSONObj& body) {
    OpMsgBuilder msgBuilder;
    msgBuilder.setBody(body);
    auto msg = msgBuilder.finishWithoutSizeChecking();
    // testOneInput expects a buffer containing a message without a size, so remove
    // the size added by msgBuilder.
    return fixture.testOneInput(msg.buf() + sizeof(int), msg.size() - sizeof(int));
}

TEST(OpMsgFuzzerShardFixtureTest, Ping) {
    auto fixture = OpMsgFuzzerShardFixture(/*skipGlobalInitializers=*/true);
    ASSERT_EQ(runCommand(fixture, fromjson(R"({ ping: 1, $db: "admin" })")), 0);
    // In non-debug builds, ping can pass even when the defaults are missing, so check for them.
    ASSERT(ReadWriteConcernDefaults::getDecoration(getGlobalServiceContext()->getService()));
}

TEST(OpMsgFuzzerShardFixtureTest, Hello) {
    auto fixture = OpMsgFuzzerShardFixture(/*skipGlobalInitializers=*/true);
    ASSERT_EQ(runCommand(fixture, fromjson(R"({ hello: 1, $db: "admin" })")), 0);
}

}  // namespace
}  // namespace mongo
