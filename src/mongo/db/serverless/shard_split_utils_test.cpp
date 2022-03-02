/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/repl/repl_set_config_test.h"
#include "mongo/db/serverless/shard_split_utils.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {
TEST(MakeSplitConfig, toBSONRoundTripAbility) {
    ReplSetConfig configA;
    ReplSetConfig configB;
    const std::string recipientTagName{"recipient"};
    const auto donorReplSetId = OID::gen();
    const auto recipientMemberBSON =
        BSON("_id" << 1 << "host"
                   << "localhost:20002"
                   << "priority" << 0 << "votes" << 0 << "tags" << BSON(recipientTagName << "one"));

    configA = ReplSetConfig::parse(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "localhost:12345")
                                                      << recipientMemberBSON)
                                        << "settings"
                                        << BSON("heartbeatIntervalMillis"
                                                << 5000 << "heartbeatTimeoutSecs" << 20
                                                << "replicaSetId" << donorReplSetId)));
    configB = ReplSetConfig::parse(configA.toBSON());
    ASSERT_TRUE(configA == configB);

    // here we will test that the result from the method `makeSplitConfig` matches the hardcoded
    // resultSplitConfigBSON. We will also check that the recipient from the splitConfig matches
    // the hardcoded recipientConfig.
    const std::string recipientConfigSetName{"newSet"};
    BSONObj resultRecipientConfigBSON = BSON(
        "_id" << recipientConfigSetName << "version" << 2 << "protocolVersion" << 1 << "members"
              << BSON_ARRAY(BSON("_id" << 0 << "host"
                                       << "localhost:20002"
                                       << "priority" << 1 << "votes" << 1 << "tags"
                                       << BSON(recipientTagName << "one")))
              << "settings"
              << BSON("heartbeatIntervalMillis" << 5000 << "heartbeatTimeoutSecs" << 20));

    BSONObj resultSplitConfigBSON = BSON("_id"
                                         << "rs0"
                                         << "version" << 2 << "protocolVersion" << 1 << "members"
                                         << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                  << "localhost:12345"))
                                         << "settings"
                                         << BSON("heartbeatIntervalMillis"
                                                 << 5000 << "heartbeatTimeoutSecs" << 20
                                                 << "replicaSetId" << donorReplSetId)
                                         << "recipientConfig" << resultRecipientConfigBSON);

    const ReplSetConfig splitConfigResult =
        serverless::makeSplitConfig(configA, recipientConfigSetName, recipientTagName);

    ASSERT_OK(splitConfigResult.validate());
    ASSERT_TRUE(splitConfigResult == ReplSetConfig::parse(splitConfigResult.toBSON()));

    auto resultSplitConfig = ReplSetConfig::parse(resultSplitConfigBSON);
    ASSERT_OK(resultSplitConfig.validate());
    ASSERT_TRUE(splitConfigResult == resultSplitConfig);

    auto recipientConfigResultPtr = splitConfigResult.getRecipientConfig();
    // we use getReplicaSetId to match the newly replicaSetId created from makeSplitConfig on the
    // recipientConfig since configA had a replicaSetId in its config.

    ASSERT_TRUE(*recipientConfigResultPtr == ReplSetConfig::parse(resultRecipientConfigBSON));
}

TEST(MakeSplitConfig, ValidateSplitConfigIntegrityTest) {
    const std::string recipientTagName{"recipient"};
    const std::string donorConfigSetName{"rs0"};
    const std::string recipientConfigSetName{"newSet"};
    const ReplSetConfig config = ReplSetConfig::parse(
        BSON("_id" << donorConfigSetName << "version" << 1 << "protocolVersion" << 1 << "members"
                   << BSON_ARRAY(BSON("_id" << 0 << "host"
                                            << "localhost:20001"
                                            << "priority" << 1 << "tags"
                                            << BSON("NYC"
                                                    << "NY"))
                                 << BSON("_id" << 1 << "host"
                                               << "localhost:20002"
                                               << "priority" << 0 << "votes" << 0 << "tags"
                                               << BSON(recipientTagName << "one"))
                                 << BSON("_id" << 2 << "host"
                                               << "localhost:20003"
                                               << "priority" << 6))
                   << "settings" << BSON("electionTimeoutMillis" << 1000)));


    const ReplSetConfig splitConfig =
        serverless::makeSplitConfig(config, recipientConfigSetName, recipientTagName);
    ASSERT_OK(splitConfig.validate());
    ASSERT_EQ(splitConfig.getReplSetName(), donorConfigSetName);
    ASSERT_TRUE(splitConfig.toBSON().hasField("members"));
    ASSERT_EQUALS(2, splitConfig.getNumMembers());
    ASSERT_TRUE(splitConfig.isSplitConfig());

    auto recipientConfigPtr = splitConfig.getRecipientConfig();
    ASSERT_OK(recipientConfigPtr->validate());
    ASSERT_TRUE(recipientConfigPtr->toBSON().hasField("members"));
    ASSERT_EQUALS(1, recipientConfigPtr->getNumMembers());

    ASSERT_FALSE(recipientConfigPtr->isSplitConfig());
    ASSERT_TRUE(recipientConfigPtr->getRecipientConfig() == nullptr);
    ASSERT_EQ(recipientConfigPtr->getReplSetName(), recipientConfigSetName);

    ASSERT_THROWS_CODE(
        serverless::makeSplitConfig(splitConfig, recipientConfigSetName, recipientTagName),
        AssertionException,
        6201800 /*calling on a splitconfig*/);
}

TEST(MakeSplitConfig, SplitConfigAssertionsTest) {
    const std::string recipientConfigSetName{"newSet"};
    const std::string recipientTagName{"recipient"};
    auto baseConfigBSON = BSON("_id"
                               << "rs0"
                               << "version" << 1 << "protocolVersion" << 1 << "members"
                               << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                        << "localhost:20002"
                                                        << "priority" << 0 << "votes" << 0)));

    ASSERT_THROWS_CODE(serverless::makeSplitConfig(ReplSetConfig::parse(baseConfigBSON),
                                                   recipientConfigSetName,
                                                   recipientTagName),
                       AssertionException,
                       6201801 /*no recipient members created*/);

    baseConfigBSON = BSON("_id"
                          << "rs0"
                          << "version" << 1 << "protocolVersion" << 1 << "members"
                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                   << "localhost:20002"
                                                   << "priority" << 0 << "votes" << 0 << "tags"
                                                   << BSON(recipientTagName << "one")))
                          << "settings" << BSON("electionTimeoutMillis" << 1000));

    ASSERT_THROWS_CODE(serverless::makeSplitConfig(ReplSetConfig::parse(baseConfigBSON),
                                                   recipientConfigSetName,
                                                   recipientTagName),
                       AssertionException,
                       6201802 /*no donor members created*/);
}
}  // namespace
}  // namespace repl
}  // namespace mongo
