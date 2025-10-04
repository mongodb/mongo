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

#include "mongo/db/repl/member_config.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/repl/member_id.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <ostream>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {
namespace {

TEST(MemberConfig, ParseMinimalMemberConfigAndCheckDefaults) {
    ReplSetTagConfig tagConfig;
    MemberConfig mc(BSON("_id" << 0 << "host"
                               << "localhost:12345"),
                    &tagConfig);
    ASSERT_EQUALS(MemberId(0), mc.getId());
    ASSERT_EQUALS(HostAndPort("localhost", 12345), mc.getHostAndPort());
    ASSERT_EQUALS(1.0, mc.getPriority());
    ASSERT_EQUALS(Seconds(0), mc.getSecondaryDelay());
    ASSERT_TRUE(mc.isVoter());
    ASSERT_FALSE(mc.isHidden());
    ASSERT_FALSE(mc.isArbiter());
    ASSERT_FALSE(mc.isNewlyAdded());
    ASSERT_TRUE(mc.shouldBuildIndexes());
    ASSERT_EQUALS(5U, mc.getNumTags());
    ASSERT_FALSE(mc.hasTags());
}

TEST(MemberConfig, ParseFailsWithIllegalFieldName) {
    ReplSetTagConfig tagConfig;
    ASSERT_THROWS_CODE(MemberConfig(BSON("_id" << 0 << "host"
                                               << "localhost"
                                               << "frim" << 1),
                                    &tagConfig),
                       AssertionException,
                       ErrorCodes::IDLUnknownField);
}

TEST(MemberConfig, ParseFailsWithMissingIdField) {
    ReplSetTagConfig tagConfig;
    ASSERT_THROWS_CODE(MemberConfig(BSON("host" << "localhost:12345"), &tagConfig),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST(MemberConfig, ParseMemberConfigId) {
    ReplSetTagConfig tagConfig;
    // Fail case
    {
        ASSERT_THROWS(MemberConfig(BSON("_id" << -1 << "host"
                                              << "localhost:12345"),
                                   &tagConfig),
                      ExceptionFor<ErrorCodes::BadValue>);
    }
    // Pass cases
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "localhost:12345"),
                        &tagConfig);
        ASSERT_EQUALS(MemberId(0), mc.getId());
    }
    {
        MemberConfig mc(BSON("_id" << 255 << "host"
                                   << "localhost:12345"),
                        &tagConfig);
        ASSERT_EQUALS(MemberId(255), mc.getId());
    }
    {
        MemberConfig mc(BSON("_id" << 256 << "host"
                                   << "localhost:12345"),
                        &tagConfig);
        ASSERT_EQUALS(MemberId(256), mc.getId());
    }
}

TEST(MemberConfig, ParseFailsWithBadIdField) {
    ReplSetTagConfig tagConfig;
    ASSERT_THROWS_CODE(MemberConfig(BSON("host" << "localhost:12345"), &tagConfig),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
    ASSERT_THROWS(MemberConfig(BSON("_id" << "0"
                                          << "host"
                                          << "localhost:12345"),
                               &tagConfig),
                  ExceptionFor<ErrorCodes::TypeMismatch>);
    ASSERT_THROWS(MemberConfig(BSON("_id" << Date_t() << "host"
                                          << "localhost:12345"),
                               &tagConfig),
                  ExceptionFor<ErrorCodes::TypeMismatch>);
}

TEST(MemberConfig, ParseAcceptsAnyNumberId) {
    ReplSetTagConfig tagConfig;
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"),
                        &tagConfig);
        ASSERT_EQUALS(mc.getId(), MemberId(0));
    }
    {
        MemberConfig mc(BSON("_id" << 0.5 << "host"
                                   << "h"),
                        &tagConfig);
        ASSERT_EQUALS(mc.getId(), MemberId(0));
    }
    {
        MemberConfig mc(BSON("_id" << -0.5 << "host"
                                   << "h"),
                        &tagConfig);
        ASSERT_EQUALS(mc.getId(), MemberId(0));
    }
    {
        MemberConfig mc(BSON("_id" << 1 << "host"
                                   << "h"),
                        &tagConfig);
        ASSERT_EQUALS(mc.getId(), MemberId(1));
    }
    {
        ASSERT_THROWS(MemberConfig(BSON("_id" << -1.5 << "host"
                                              << "localhost:12345"),
                                   &tagConfig),
                      ExceptionFor<ErrorCodes::BadValue>);
    }
    {
        MemberConfig mc(BSON("_id" << 1.6 << "host"
                                   << "h"),
                        &tagConfig);
        ASSERT_EQUALS(mc.getId(), MemberId(1));
    }
    {
        MemberConfig mc(BSON("_id" << 4 << "host"
                                   << "h"),
                        &tagConfig);
        ASSERT_EQUALS(mc.getId(), MemberId(4));
    }
}

TEST(MemberConfig, ParseFailsWithMissingHostField) {
    ReplSetTagConfig tagConfig;
    ASSERT_THROWS_CODE(MemberConfig(BSON("_id" << 0), &tagConfig),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST(MemberConfig, ParseFailsWithBadHostField) {
    ReplSetTagConfig tagConfig;
    ASSERT_THROWS(MemberConfig(BSON("_id" << 0 << "host" << 0), &tagConfig),
                  ExceptionFor<ErrorCodes::TypeMismatch>);
    ASSERT_THROWS(MemberConfig(BSON("_id" << 0 << "host"
                                          << ""),
                               &tagConfig),
                  ExceptionFor<ErrorCodes::FailedToParse>);
    ASSERT_THROWS(MemberConfig(BSON("_id" << 0 << "host"
                                          << "myhost:zabc"),
                               &tagConfig),
                  ExceptionFor<ErrorCodes::FailedToParse>);
}

TEST(MemberConfig, ParseTrimsHostname) {
    ReplSetTagConfig tagConfig;
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "localhost:12345 "),
                        &tagConfig);
        ASSERT_EQUALS(HostAndPort("localhost", 12345), mc.getHostAndPort());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h "),
                        &tagConfig);
        ASSERT_EQUALS(HostAndPort("h", 27017), mc.getHostAndPort());
    }
}

TEST(MemberConfig, ParseArbiterOnly) {
    ReplSetTagConfig tagConfig;
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "arbiterOnly" << 1.0),
                        &tagConfig);
        ASSERT_TRUE(mc.isArbiter());
        ASSERT_EQUALS(0.0, mc.getPriority());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "arbiterOnly" << false),
                        &tagConfig);
        ASSERT_TRUE(!mc.isArbiter());
        ASSERT_EQUALS(1.0, mc.getPriority());
    }
}
}  // namespace

void setNewlyAdded_forTest(MemberConfig* mc, boost::optional<bool> newlyAdded) {
    mc->setNewlyAdded(newlyAdded);
}

namespace {
TEST(MemberConfig, ParseAndSetNewlyAddedField) {
    ReplSetTagConfig tagConfig;
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"),
                        &tagConfig);
        // Verify that the 'newlyAdded' field is not added by default.
        ASSERT_FALSE(mc.isNewlyAdded());

        setNewlyAdded_forTest(&mc, true);
        ASSERT_TRUE(mc.isNewlyAdded());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "newlyAdded" << true),
                        &tagConfig);
        ASSERT_TRUE(mc.isNewlyAdded());
    }
}

TEST(MemberConfig, NewlyAddedSetToFalseShouldThrow) {
    ReplSetTagConfig tagConfig;
    ASSERT_THROWS(MemberConfig(BSON("_id" << 0 << "host"
                                          << "h"
                                          << "newlyAdded" << false),
                               &tagConfig),
                  ExceptionFor<ErrorCodes::InvalidReplicaSetConfig>);
}

TEST(MemberConfig, VotingNodeWithNewlyAddedFieldShouldStillHaveVoteAfterToBSON) {
    ReplSetTagConfig tagConfig;

    // Create a member with 'newlyAdded: true'.
    MemberConfig mc(BSON("_id" << 0 << "host"
                               << "h"
                               << "newlyAdded" << true),
                    &tagConfig);
    ASSERT_TRUE(mc.isNewlyAdded());

    // Nodes with newly added field set should transiently not be allowed to vote.
    ASSERT_FALSE(mc.isVoter());
    ASSERT_EQ(0, mc.getNumVotes());

    // Verify that the member is still a voting node.
    const auto obj = mc.toBSON();
    long long votes;
    uassertStatusOK(bsonExtractIntegerField(obj, "votes", &votes));
    ASSERT_EQ(1, votes);

    MemberConfig mc2(obj, &tagConfig);
    ASSERT_TRUE(mc2.isNewlyAdded());
    ASSERT_FALSE(mc2.isVoter());
    ASSERT_EQ(0, mc2.getNumVotes());
}

TEST(MemberConfig, NonVotingNodesWithNewlyAddedFieldShouldStillHaveZeroVotesAfterToBSON) {
    ReplSetTagConfig tagConfig;

    MemberConfig mc(BSON("_id" << 0 << "host"
                               << "h"
                               << "votes" << 0 << "priority" << 0 << "newlyAdded" << true),
                    &tagConfig);
    ASSERT_TRUE(mc.isNewlyAdded());
    ASSERT_FALSE(mc.isVoter());
    ASSERT_EQ(0, mc.getNumVotes());

    const auto obj = mc.toBSON();
    long long votes;
    uassertStatusOK(bsonExtractIntegerField(obj, "votes", &votes));
    ASSERT_EQ(0, votes);

    MemberConfig mc2(obj, &tagConfig);
    ASSERT_TRUE(mc2.isNewlyAdded());
    ASSERT_FALSE(mc2.isVoter());
    ASSERT_EQ(0, mc2.getNumVotes());
}

TEST(MemberConfig, NonVotingNodesShouldStillHaveZeroVotesAfterToBSON) {
    ReplSetTagConfig tagConfig;
    MemberConfig mc(BSON("_id" << 0 << "host"
                               << "h"
                               << "votes" << 0 << "priority" << 0),
                    &tagConfig);
    ASSERT_FALSE(mc.isNewlyAdded());
    ASSERT_FALSE(mc.isVoter());
    ASSERT_EQ(0, mc.getNumVotes());

    const auto obj = mc.toBSON();
    long long votes;
    uassertStatusOK(bsonExtractIntegerField(obj, "votes", &votes));
    ASSERT_EQ(0, votes);

    MemberConfig mc2(obj, &tagConfig);
    ASSERT_FALSE(mc2.isNewlyAdded());
    ASSERT_FALSE(mc2.isVoter());
    ASSERT_EQ(0, mc2.getNumVotes());
}

TEST(MemberConfig, VotingNodesShouldStillHaveVoteAfterToBSON) {
    ReplSetTagConfig tagConfig;
    MemberConfig mc(BSON("_id" << 0 << "host"
                               << "h"),
                    &tagConfig);
    ASSERT_FALSE(mc.isNewlyAdded());
    ASSERT_TRUE(mc.isVoter());
    ASSERT_EQ(1, mc.getNumVotes());

    const auto obj = mc.toBSON();
    long long votes;
    uassertStatusOK(bsonExtractIntegerField(obj, "votes", &votes));
    ASSERT_EQ(1, votes);

    MemberConfig mc2(obj, &tagConfig);
    ASSERT_FALSE(mc2.isNewlyAdded());
    ASSERT_TRUE(mc2.isVoter());
    ASSERT_EQ(1, mc2.getNumVotes());
}

TEST(MemberConfig, NodeWithNewlyAddedFieldShouldStillHavePriorityAfterToBSON) {
    ReplSetTagConfig tagConfig;

    // Create a member with 'newlyAdded: true' and 'priority: 3'.
    MemberConfig mc(BSON("_id" << 0 << "host"
                               << "h"
                               << "newlyAdded" << true << "priority" << 3),
                    &tagConfig);
    ASSERT_TRUE(mc.isNewlyAdded());

    // Nodes with newly added field set should transiently have 'priority: 0'.
    ASSERT_EQ(0, mc.getPriority());

    // Verify that 'toBSON()' returns the underlying priority field.
    const auto obj = mc.toBSON();
    long long priority;
    uassertStatusOK(bsonExtractIntegerField(obj, "priority", &priority));
    ASSERT_EQ(3, priority);

    MemberConfig mc2(obj, &tagConfig);
    ASSERT_TRUE(mc2.isNewlyAdded());
    ASSERT_EQ(0, mc2.getPriority());
}

TEST(MemberConfig, PriorityZeroNodeWithNewlyAddedFieldShouldStillHaveZeroPriorityAfterToBSON) {
    ReplSetTagConfig tagConfig;

    // Create a member with 'newlyAdded: true' and 'priority: 0'.
    MemberConfig mc(BSON("_id" << 0 << "host"
                               << "h"
                               << "newlyAdded" << true << "votes" << 0 << "priority" << 0),
                    &tagConfig);
    ASSERT_TRUE(mc.isNewlyAdded());

    // Nodes with newly added field set should transiently have 'priority: 0'.
    ASSERT_EQ(0, mc.getPriority());

    // Verify that 'toBSON()' returns the underlying priority field.
    const auto obj = mc.toBSON();
    long long priority;
    uassertStatusOK(bsonExtractIntegerField(obj, "priority", &priority));
    ASSERT_EQ(0, priority);

    MemberConfig mc2(obj, &tagConfig);
    ASSERT_TRUE(mc2.isNewlyAdded());
    ASSERT_EQ(0, mc2.getPriority());
}

TEST(MemberConfig, PriorityZeroNodeShouldStillHaveZeroPriorityAfterToBSON) {
    ReplSetTagConfig tagConfig;

    // Create a member with 'priority: 0.
    MemberConfig mc(BSON("_id" << 0 << "host"
                               << "h"
                               << "votes" << 0 << "priority" << 0),
                    &tagConfig);

    ASSERT_FALSE(mc.isNewlyAdded());

    // When the node does not have its 'newlyAdded' field set, the effective priority should equal
    // the underlying priority field.
    ASSERT_EQ(0, mc.getPriority());

    // Verify that 'toBSON()' returns the underlying priority field.
    const auto obj = mc.toBSON();
    long long priority;
    uassertStatusOK(bsonExtractIntegerField(obj, "priority", &priority));
    ASSERT_EQ(0, priority);

    MemberConfig mc2(obj, &tagConfig);
    ASSERT_FALSE(mc2.isNewlyAdded());
    ASSERT_EQ(0, mc2.getPriority());
}

TEST(MemberConfig, NodeShouldStillHavePriorityAfterToBSON) {
    ReplSetTagConfig tagConfig;

    // Create a member with 'priority: 0.
    MemberConfig mc(BSON("_id" << 0 << "host"
                               << "h"
                               << "priority" << 3),
                    &tagConfig);

    ASSERT_FALSE(mc.isNewlyAdded());

    // When the node does not have its 'newlyAdded' field set, the effective priority should equal
    // the underlying priority field.
    ASSERT_EQ(3, mc.getPriority());

    // Verify that 'toBSON()' returns the underlying priority field.
    const auto obj = mc.toBSON();
    long long priority;
    uassertStatusOK(bsonExtractIntegerField(obj, "priority", &priority));
    ASSERT_EQ(3, priority);

    MemberConfig mc2(obj, &tagConfig);
    ASSERT_FALSE(mc2.isNewlyAdded());
    ASSERT_EQ(3, mc2.getPriority());
}

TEST(MemberConfig, CanOmitNewlyAddedFieldInBSONViaToBSONWithoutNewlyAdded) {
    ReplSetTagConfig tagConfig;

    // Create a member with 'newlyAdded: true' and 'priority: 0'.
    MemberConfig mc(BSON("_id" << 0 << "host"
                               << "h"
                               << "newlyAdded" << true << "priority" << 0),
                    &tagConfig);
    ASSERT_TRUE(mc.isNewlyAdded());

    // Verify that the normal 'toBSON()' return includes the 'newlyAdded' field.
    auto obj = mc.toBSON();
    bool newlyAdded;
    uassertStatusOK(bsonExtractBooleanField(obj, "newlyAdded", &newlyAdded));
    ASSERT_EQ(true, newlyAdded);

    // Also verify that 'toBSONWithoutNewlyAdded' omits the 'newlyAdded' field.
    obj = mc.toBSON(true /* omitNewlyAddedField */);
    ASSERT_EQ(ErrorCodes::NoSuchKey, bsonExtractBooleanField(obj, "newlyAdded", &newlyAdded));

    MemberConfig mc2(obj, &tagConfig);
    ASSERT_FALSE(mc2.isNewlyAdded());
    ASSERT_EQ(0, mc2.getPriority());
}

TEST(MemberConfig, ArbiterCannotHaveNewlyAddedFieldSet) {
    ReplSetTagConfig tagConfig;

    // Verify that an exception is thrown when we try to create an arbiter with 'newlyAdded' field.
    ASSERT_THROWS(MemberConfig(BSON("_id" << 0 << "host"
                                          << "h"
                                          << "arbiterOnly" << true << "newlyAdded" << true),
                               &tagConfig),
                  ExceptionFor<ErrorCodes::BadValue>);
}

TEST(MemberConfig, ParseHidden) {
    ReplSetTagConfig tagConfig;
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 0 << "hidden" << 1.0),
                        &tagConfig);
        ASSERT_TRUE(mc.isHidden());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "hidden" << false),
                        &tagConfig);
        ASSERT_TRUE(!mc.isHidden());
    }
    ASSERT_THROWS(MemberConfig(BSON("_id" << 0 << "host"
                                          << "h"
                                          << "priority" << 0 << "hidden"
                                          << "1.0"),
                               &tagConfig),
                  ExceptionFor<ErrorCodes::TypeMismatch>);

    ASSERT_THROWS(MemberConfig(BSON("_id" << 0 << "host"
                                          << "h"
                                          << "hidden" << 1.0),
                               &tagConfig),
                  ExceptionFor<ErrorCodes::BadValue>);
}

TEST(MemberConfig, ParseBuildIndexes) {
    ReplSetTagConfig tagConfig;
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 0 << "buildIndexes" << 1.0),
                        &tagConfig);
        ASSERT_TRUE(mc.shouldBuildIndexes());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 0 << "buildIndexes" << false),
                        &tagConfig);
        ASSERT_FALSE(mc.shouldBuildIndexes());
    }

    ASSERT_THROWS(MemberConfig(BSON("_id" << 0 << "host"
                                          << "h"
                                          << "buildIndexes" << false),
                               &tagConfig),
                  ExceptionFor<ErrorCodes::BadValue>);
}

TEST(MemberConfig, ParseVotes) {
    ReplSetTagConfig tagConfig;
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "votes" << 1.0),
                        &tagConfig);
        ASSERT_TRUE(mc.isVoter());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "votes" << 0 << "priority" << 0),
                        &tagConfig);
        ASSERT_FALSE(mc.isVoter());
    }
    {
        // For backwards compatibility, truncate 1.X to 1, and 0.X to 0 (and -0.X to 0).
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "votes" << 1.5),
                        &tagConfig);
        ASSERT_TRUE(mc.isVoter());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "votes" << 0.5 << "priority" << 0),
                        &tagConfig);
        ASSERT_FALSE(mc.isVoter());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "votes" << -0.5 << "priority" << 0),
                        &tagConfig);
        ASSERT_FALSE(mc.isVoter());
    }
    {
        ASSERT_THROWS_CODE(MemberConfig(BSON("_id" << 0 << "host"
                                                   << "h"
                                                   << "votes" << 2 << "priority" << 0),
                                        &tagConfig),
                           AssertionException,
                           ErrorCodes::BadValue);
    }
    {
        ASSERT_THROWS(MemberConfig(BSON("_id" << 0 << "host"
                                              << "h"
                                              << "votes" << Date_t::fromMillisSinceEpoch(2)),
                                   &tagConfig),
                      ExceptionFor<ErrorCodes::TypeMismatch>);
    }
    {
        ASSERT_THROWS(MemberConfig(BSON("_id" << 0 << "host"
                                              << "h"
                                              << "votes" << 0),
                                   &tagConfig),
                      ExceptionFor<ErrorCodes::BadValue>);
    }
    {
        ASSERT_THROWS_CODE(MemberConfig(BSON("_id" << 0 << "host"
                                                   << "h"
                                                   << "votes" << -1 << "priority" << 0),
                                        &tagConfig),
                           AssertionException,
                           ErrorCodes::BadValue);
    }
}

TEST(MemberConfig, ParsePriority) {
    ReplSetTagConfig tagConfig;
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 1),
                        &tagConfig);
        ASSERT_EQUALS(1.0, mc.getPriority());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 0),
                        &tagConfig);
        ASSERT_EQUALS(0.0, mc.getPriority());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 100.8),
                        &tagConfig);
        ASSERT_EQUALS(100.8, mc.getPriority());
    }
    {
        ASSERT_THROWS(MemberConfig(BSON("_id" << 0 << "host"
                                              << "h"
                                              << "priority" << Date_t::fromMillisSinceEpoch(2)),
                                   &tagConfig),
                      ExceptionFor<ErrorCodes::TypeMismatch>);
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 1000),
                        &tagConfig);
        ASSERT_EQUALS(1000, mc.getPriority());
    }
    {
        ASSERT_THROWS_CODE(MemberConfig(BSON("_id" << 0 << "host"
                                                   << "h"
                                                   << "priority" << -1),
                                        &tagConfig),
                           AssertionException,
                           ErrorCodes::BadValue);
    }
    {
        ASSERT_THROWS_CODE(MemberConfig(BSON("_id" << 0 << "host"
                                                   << "h"
                                                   << "priority" << 1001),
                                        &tagConfig),
                           AssertionException,
                           ErrorCodes::BadValue);
    }
}

TEST(MemberConfig, ParseSecondaryDelay) {
    ReplSetTagConfig tagConfig;
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 0 << "secondaryDelaySecs" << 100),
                        &tagConfig);
        ASSERT_EQUALS(Seconds(100), mc.getSecondaryDelay());
    }
    {
        ASSERT_THROWS(MemberConfig(BSON("_id" << 0 << "host"
                                              << "h"
                                              << "secondaryDelaySecs" << 100),
                                   &tagConfig),
                      ExceptionFor<ErrorCodes::BadValue>);
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 0 << "secondaryDelaySecs" << 0),
                        &tagConfig);
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 0 << "secondaryDelaySecs" << 3600 * 10),
                        &tagConfig);
    }
    {
        ASSERT_THROWS_CODE(
            MemberConfig(BSON("_id" << 0 << "host"
                                    << "h"
                                    << "priority" << 0 << "secondaryDelaySecs" << -1),
                         &tagConfig),
            AssertionException,
            ErrorCodes::BadValue);
    }
    {
        ASSERT_THROWS_CODE(
            MemberConfig(BSON("_id" << 0 << "host"
                                    << "h"
                                    << "priority" << 0 << "secondaryDelaySecs" << 3600 * 24 * 400),
                         &tagConfig),
            AssertionException,
            ErrorCodes::BadValue);
    }
}

TEST(MemberConfig, ParseAcceptsAnyNumberSecondaryDelay) {
    ReplSetTagConfig tagConfig;
    {
        MemberConfig mc(BSON("_id" << 1 << "host"
                                   << "h"
                                   << "priority" << 0 << "secondaryDelaySecs" << 0),
                        &tagConfig);
        ASSERT_EQUALS(mc.getSecondaryDelay(), Seconds(0));
    }
    {
        MemberConfig mc(BSON("_id" << 1 << "host"
                                   << "h"
                                   << "priority" << 0 << "secondaryDelaySecs" << 0.5),
                        &tagConfig);
        ASSERT_EQUALS(mc.getSecondaryDelay(), Seconds(0));
    }
    {
        MemberConfig mc(BSON("_id" << 1 << "host"
                                   << "h"
                                   << "priority" << 0 << "secondaryDelaySecs" << -0.5),
                        &tagConfig);
        ASSERT_EQUALS(mc.getSecondaryDelay(), Seconds(0));
    }
    {
        MemberConfig mc(BSON("_id" << 1 << "host"
                                   << "h"
                                   << "priority" << 0 << "secondaryDelaySecs" << 1),
                        &tagConfig);
        ASSERT_EQUALS(mc.getSecondaryDelay(), Seconds(1));
    }
    {
        ASSERT_THROWS_CODE(
            MemberConfig(BSON("_id" << 1 << "host"
                                    << "h"
                                    << "priority" << 0 << "secondaryDelaySecs" << -1.5),
                         &tagConfig),
            AssertionException,
            ErrorCodes::BadValue);
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 0 << "secondaryDelaySecs" << 1.6),
                        &tagConfig);
        ASSERT_EQUALS(mc.getSecondaryDelay(), Seconds(1));
    }
    {
        MemberConfig mc(BSON("_id" << 1 << "host"
                                   << "h"
                                   << "priority" << 0 << "secondaryDelaySecs" << 4),
                        &tagConfig);
        ASSERT_EQUALS(mc.getSecondaryDelay(), Seconds(4));
    }
}

TEST(MemberConfig, ParseTags) {
    ReplSetTagConfig tagConfig;
    MemberConfig mc(BSON("_id" << 0 << "host"
                               << "h"
                               << "tags"
                               << BSON("k1" << "v1"
                                            << "k2"
                                            << "v2")),
                    &tagConfig);
    ASSERT_TRUE(mc.hasTags());
    ASSERT_EQUALS(7U, mc.getNumTags());
    ASSERT_EQUALS(7, std::distance(mc.tagsBegin(), mc.tagsEnd()));
    ASSERT_EQUALS(1, std::count(mc.tagsBegin(), mc.tagsEnd(), tagConfig.findTag("k1", "v1")));
    ASSERT_EQUALS(1, std::count(mc.tagsBegin(), mc.tagsEnd(), tagConfig.findTag("k2", "v2")));
    ASSERT_EQUALS(1, std::count(mc.tagsBegin(), mc.tagsEnd(), tagConfig.findTag("$voter", "0")));
    ASSERT_EQUALS(1,
                  std::count(mc.tagsBegin(), mc.tagsEnd(), tagConfig.findTag("$electable", "0")));
    ASSERT_EQUALS(1, std::count(mc.tagsBegin(), mc.tagsEnd(), tagConfig.findTag("$all", "0")));
    ASSERT_EQUALS(1,
                  std::count(mc.tagsBegin(), mc.tagsEnd(), tagConfig.findTag("$configAll", "0")));
    ASSERT_EQUALS(1,
                  std::count(mc.tagsBegin(), mc.tagsEnd(), tagConfig.findTag("$configVoter", "0")));
}

TEST(MemberConfig, ParseEmptyTags) {
    ReplSetTagConfig tagConfig;
    MemberConfig mc(BSON("_id" << 0 << "host"
                               << "localhost:12345"
                               << "tags" << BSONObj()),
                    &tagConfig);
    ASSERT_EQUALS(5U, mc.getNumTags());
    ASSERT_FALSE(mc.hasTags());
    BSONObj obj = mc.toBSON();
    BSONElement tagField = obj.getField("tags");
    ASSERT_TRUE(tagField.type() == BSONType::object);
    ASSERT_TRUE(tagField.Obj().isEmpty());
}

TEST(MemberConfig, TagsSerializedWhenMissing) {
    ReplSetTagConfig tagConfig;
    MemberConfig mc(BSON("_id" << 0 << "host"
                               << "localhost:12345"),
                    &tagConfig);
    BSONObj obj = mc.toBSON();
    BSONElement tagField = obj.getField("tags");
    ASSERT_TRUE(tagField.type() == BSONType::object);
    ASSERT_TRUE(tagField.Obj().isEmpty());
}

TEST(MemberConfig, ParseHorizonFields) {
    ReplSetTagConfig tagConfig;
    MemberConfig mc(BSON("_id" << 0 << "host"
                               << "h"
                               << "horizons"
                               << BSON("alpha" << "a.host:43"
                                               << "beta"
                                               << "b.host:256")),
                    &tagConfig);

    ASSERT_EQUALS(std::size_t{1}, mc.getHorizonMappings().count("alpha"));
    ASSERT_EQUALS(std::size_t{1}, mc.getHorizonMappings().count("beta"));
    ASSERT_EQUALS(std::size_t{1}, mc.getHorizonMappings().count("__default"));

    ASSERT_EQUALS("alpha", mc.getHorizonReverseHostMappings().find("a.host")->second);
    ASSERT_EQUALS("beta", mc.getHorizonReverseHostMappings().find("b.host")->second);
    ASSERT_EQUALS("__default", mc.getHorizonReverseHostMappings().find("h")->second);

    ASSERT_EQUALS(HostAndPort("a.host", 43), mc.getHorizonMappings().find("alpha")->second);
    ASSERT_EQUALS(HostAndPort("b.host", 256), mc.getHorizonMappings().find("beta")->second);
    ASSERT_EQUALS(HostAndPort("h"), mc.getHorizonMappings().find("__default")->second);

    ASSERT_EQUALS(mc.getHorizonMappings().size(), std::size_t{3});
}

TEST(MemberConfig, DuplicateHorizonNames) {
    ReplSetTagConfig tagConfig;
    try {
        MemberConfig mem(BSON("_id" << 0 << "host"
                                    << "h"
                                    << "horizons"
                                    << BSON("goofyRepeatedHorizonName" << "a.host:43"
                                                                       << "goofyRepeatedHorizonName"

                                                                       << "b.host:256")),
                         &tagConfig);
        ASSERT_TRUE(false);  // Should not succeed.
    } catch (const ExceptionFor<ErrorCodes::BadValue>& ex) {
        const Status& s = ex.toStatus();
        ASSERT_NOT_EQUALS(s.reason().find("goofyRepeatedHorizonName"), std::string::npos);
        ASSERT_NOT_EQUALS(s.reason().find("Duplicate horizon name found"), std::string::npos);
    }
    try {
        MemberConfig mem(
            BSON("_id" << 0 << "host"
                       << "h"
                       << "horizons"
                       << BSON("someUniqueHorizonName"
                               << "a.host:43" << SplitHorizon::kDefaultHorizon << "b.host:256")),
            &tagConfig);
        ASSERT_TRUE(false);  // Should not succeed.
    } catch (const ExceptionFor<ErrorCodes::BadValue>& ex) {
        const Status& s = ex.toStatus();
        ASSERT_NOT_EQUALS(s.reason().find(std::string(SplitHorizon::kDefaultHorizon)),
                          std::string::npos);
        ASSERT_NOT_EQUALS(s.reason().find("reserved for internal"), std::string::npos);
    }
}

TEST(MemberConfig, DuplicateHorizonHostAndPort) {
    ReplSetTagConfig tagConfig;
    // Repeated `HostAndPort` within the horizon definition.
    try {
        MemberConfig mem(BSON("_id" << 0 << "host"
                                    << "uniquehostname.example.com:42"
                                    << "horizons"
                                    << BSON("alpha" << "duplicatedhostname.example.com:42"
                                                    << "beta"
                                                    << "duplicatedhostname.example.com:42")),
                         &tagConfig);
        ASSERT_TRUE(false);  // Should not succeed.
    } catch (const ExceptionFor<ErrorCodes::BadValue>& ex) {
        const Status& s = ex.toStatus();
        ASSERT_EQUALS(s.reason().find("uniquehostname.example.com"), std::string::npos);
        ASSERT_NOT_EQUALS(s.reason().find("duplicatedhostname.example.com"), std::string::npos)
            << "Failed to find duplicated host name in message: " << s.reason();
    }

    // Repeated `HostAndPort` across the host and members.
    try {
        MemberConfig mem(BSON("_id" << 0 << "host"
                                    << "duplicatedhostname.example.com:42"
                                    << "horizons"
                                    << BSON("alpha" << "uniquehostname.example.com:42"
                                                    << "beta"
                                                    << "duplicatedhostname.example.com:42")),
                         &tagConfig);
        ASSERT_TRUE(false);  // Should not succeed.
    } catch (const ExceptionFor<ErrorCodes::BadValue>& ex) {
        const Status& s = ex.toStatus();
        ASSERT_EQUALS(s.reason().find("uniquehostname.example.com"), std::string::npos);
        ASSERT_NOT_EQUALS(s.reason().find("duplicatedhostname.example.com"), std::string::npos);
    }

    // Repeated hostname across host and horizons, with different ports should fail.
    try {
        MemberConfig mem(BSON("_id" << 0 << "host"
                                    << "duplicatedhostname.example.com:42"
                                    << "horizons"
                                    << BSON("alpha" << "uniquehostname.example.com:43"
                                                    << "beta"
                                                    << "duplicatedhostname.example.com:43")),
                         &tagConfig);
        ASSERT_TRUE(false);  // Should not succeed.
    } catch (const ExceptionFor<ErrorCodes::BadValue>& ex) {
        const Status& s = ex.toStatus();
        ASSERT_EQUALS(s.reason().find("uniquehostname.example.com"), std::string::npos);
        ASSERT_NOT_EQUALS(s.reason().find("duplicatedhostname.example.com"), std::string::npos);
    }

    // Repeated hostname within the horizons, with different ports should fail.
    try {
        MemberConfig mem(BSON("_id" << 0 << "host"
                                    << "uniquehostname.example.com:42"
                                    << "horizons"
                                    << BSON("alpha" << "duplicatedhostname.example.com:42"
                                                    << "beta"
                                                    << "duplicatedhostname.example.com:43")),
                         &tagConfig);
        ASSERT_TRUE(false);  // Should not succeed.
    } catch (const ExceptionFor<ErrorCodes::BadValue>& ex) {
        const Status& s = ex.toStatus();
        ASSERT_EQUALS(s.reason().find("uniquehostname.example.com"), std::string::npos);
        ASSERT_NOT_EQUALS(s.reason().find("duplicatedhostname.example.com"), std::string::npos);
    }
}

TEST(MemberConfig, HorizonFieldsWithNoneInSpec) {
    ReplSetTagConfig tagConfig;
    MemberConfig mc(BSON("_id" << 0 << "host"
                               << "h"),
                    &tagConfig);

    ASSERT_EQUALS(std::size_t{1}, mc.getHorizonMappings().count("__default"));

    ASSERT_EQUALS(HostAndPort("h"), mc.getHorizonMappings().find("__default")->second);

    ASSERT_EQUALS(mc.getHorizonMappings().size(), std::size_t{1});
}

TEST(MemberConfig, HorizonFieldWithEmptyStringIsRejected) {
    ReplSetTagConfig tagConfig;
    try {
        MemberConfig mem(BSON("_id" << 0 << "host"
                                    << "h"
                                    << "horizons" << BSON("" << "example.com:42")),
                         &tagConfig);
        ASSERT_TRUE(false);  // Never should get here
    } catch (const ExceptionFor<ErrorCodes::BadValue>& ex) {
        ASSERT_NOT_EQUALS(ex.toStatus().reason().find("Horizons cannot have empty names"),
                          std::string::npos);
    }
}

TEST(MemberConfig, ValidatePriorityAndSecondaryDelayRelationship) {
    ReplSetTagConfig tagConfig;
    ASSERT_THROWS(MemberConfig(BSON("_id" << 0 << "host"
                                          << "h"
                                          << "priority" << 1 << "secondaryDelaySecs" << 60),
                               &tagConfig),
                  ExceptionFor<ErrorCodes::BadValue>);
}

TEST(MemberConfig, ValidatePriorityAndHiddenRelationship) {
    ReplSetTagConfig tagConfig;
    {
        ASSERT_THROWS(MemberConfig(BSON("_id" << 0 << "host"
                                              << "h"
                                              << "priority" << 1 << "hidden" << true),
                                   &tagConfig),
                      ExceptionFor<ErrorCodes::BadValue>);
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 1 << "hidden" << false),
                        &tagConfig);
    }
}

TEST(MemberConfig, ValidatePriorityAndBuildIndexesRelationship) {
    ReplSetTagConfig tagConfig;
    {
        ASSERT_THROWS(MemberConfig(BSON("_id" << 0 << "host"
                                              << "h"
                                              << "priority" << 1 << "buildIndexes" << false),
                                   &tagConfig),
                      ExceptionFor<ErrorCodes::BadValue>);
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 1 << "buildIndexes" << true),
                        &tagConfig);
    }
}

TEST(MemberConfig, ValidateArbiterVotesRelationship) {
    ReplSetTagConfig tagConfig;
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "votes" << 1 << "arbiterOnly" << true),
                        &tagConfig);
        {
            MemberConfig mc(BSON("_id" << 0 << "host"
                                       << "h"
                                       << "votes" << 0 << "priority" << 0 << "arbiterOnly"
                                       << false),
                            &tagConfig);
        }
        {
            MemberConfig mc(BSON("_id" << 0 << "host"
                                       << "h"
                                       << "votes" << 1 << "arbiterOnly" << false),
                            &tagConfig);
        }
        {
            ASSERT_THROWS(MemberConfig(BSON("_id" << 0 << "host"
                                                  << "h"
                                                  << "votes" << 0 << "arbiterOnly" << true),
                                       &tagConfig),
                          ExceptionFor<ErrorCodes::BadValue>);
        }
    }
}

}  // namespace
}  // namespace repl
}  // namespace mongo
