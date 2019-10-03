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

#include <algorithm>
#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/db/repl/member_config.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

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
    ASSERT_EQUALS(Seconds(0), mc.getSlaveDelay());
    ASSERT_TRUE(mc.isVoter());
    ASSERT_FALSE(mc.isHidden());
    ASSERT_FALSE(mc.isArbiter());
    ASSERT_TRUE(mc.shouldBuildIndexes());
    ASSERT_EQUALS(3U, mc.getNumTags());
    ASSERT_OK(mc.validate());
}

TEST(MemberConfig, ParseFailsWithIllegalFieldName) {
    ReplSetTagConfig tagConfig;
    ASSERT_THROWS(MemberConfig(BSON("_id" << 0 << "host"
                                          << "localhost"
                                          << "frim" << 1),
                               &tagConfig),
                  ExceptionFor<ErrorCodes::BadValue>);
}

TEST(MemberConfig, ParseFailsWithMissingIdField) {
    ReplSetTagConfig tagConfig;
    ASSERT_THROWS(MemberConfig(BSON("host"
                                    << "localhost:12345"),
                               &tagConfig),
                  ExceptionFor<ErrorCodes::NoSuchKey>);
}

TEST(MemberConfig, ParseFailsWithIdOutOfRange) {
    ReplSetTagConfig tagConfig;
    {
        ASSERT_THROWS(MemberConfig(BSON("_id" << -1 << "host"
                                              << "localhost:12345"),
                                   &tagConfig),
                      ExceptionFor<ErrorCodes::BadValue>);
    }
    {
        ASSERT_THROWS(MemberConfig(BSON("_id" << -1 << "host"
                                              << "localhost:12345"),
                                   &tagConfig),
                      ExceptionFor<ErrorCodes::BadValue>);
    }
}

TEST(MemberConfig, ParseFailsWithBadIdField) {
    ReplSetTagConfig tagConfig;
    ASSERT_THROWS(MemberConfig(BSON("host"
                                    << "localhost:12345"),
                               &tagConfig),
                  ExceptionFor<ErrorCodes::NoSuchKey>);
    ASSERT_THROWS(MemberConfig(BSON("_id"
                                    << "0"
                                    << "host"
                                    << "localhost:12345"),
                               &tagConfig),
                  ExceptionFor<ErrorCodes::TypeMismatch>);
    ASSERT_THROWS(MemberConfig(BSON("_id" << Date_t() << "host"
                                          << "localhost:12345"),
                               &tagConfig),
                  ExceptionFor<ErrorCodes::TypeMismatch>);
}

TEST(MemberConfig, ParseFailsWithMissingHostField) {
    ReplSetTagConfig tagConfig;
    ASSERT_THROWS(MemberConfig(BSON("_id" << 0), &tagConfig), ExceptionFor<ErrorCodes::NoSuchKey>);
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

TEST(MemberConfig, ParseHidden) {
    ReplSetTagConfig tagConfig;
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "hidden" << 1.0),
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
                                          << "hidden"
                                          << "1.0"),
                               &tagConfig),
                  ExceptionFor<ErrorCodes::TypeMismatch>);
}

TEST(MemberConfig, ParseBuildIndexes) {
    ReplSetTagConfig tagConfig;
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "buildIndexes" << 1.0),
                        &tagConfig);
        ASSERT_TRUE(mc.shouldBuildIndexes());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "buildIndexes" << false),
                        &tagConfig);
        ASSERT_TRUE(!mc.shouldBuildIndexes());
    }
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
                                   << "votes" << 0.5),
                        &tagConfig);
        ASSERT_FALSE(mc.isVoter());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "votes" << -0.5),
                        &tagConfig);
        ASSERT_FALSE(mc.isVoter());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "votes" << 2),
                        &tagConfig);
    }
    ASSERT_THROWS(MemberConfig(BSON("_id" << 0 << "host"
                                          << "h"
                                          << "votes" << Date_t::fromMillisSinceEpoch(2)),
                               &tagConfig),
                  ExceptionFor<ErrorCodes::TypeMismatch>);
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
    ASSERT_THROWS(MemberConfig(BSON("_id" << 0 << "host"
                                          << "h"
                                          << "priority" << Date_t::fromMillisSinceEpoch(2)),
                               &tagConfig),
                  ExceptionFor<ErrorCodes::TypeMismatch>);
}

TEST(MemberConfig, ParseSlaveDelay) {
    ReplSetTagConfig tagConfig;
    MemberConfig mc(BSON("_id" << 0 << "host"
                               << "h"
                               << "slaveDelay" << 100),
                    &tagConfig);
    ASSERT_EQUALS(Seconds(100), mc.getSlaveDelay());
}

TEST(MemberConfig, ParseTags) {
    ReplSetTagConfig tagConfig;
    MemberConfig mc(BSON("_id" << 0 << "host"
                               << "h"
                               << "tags"
                               << BSON("k1"
                                       << "v1"
                                       << "k2"
                                       << "v2")),
                    &tagConfig);
    ASSERT_EQUALS(5U, mc.getNumTags());
    ASSERT_EQUALS(5, std::distance(mc.tagsBegin(), mc.tagsEnd()));
    ASSERT_EQUALS(1, std::count(mc.tagsBegin(), mc.tagsEnd(), tagConfig.findTag("k1", "v1")));
    ASSERT_EQUALS(1, std::count(mc.tagsBegin(), mc.tagsEnd(), tagConfig.findTag("k2", "v2")));
    ASSERT_EQUALS(1, std::count(mc.tagsBegin(), mc.tagsEnd(), tagConfig.findTag("$voter", "0")));
    ASSERT_EQUALS(1,
                  std::count(mc.tagsBegin(), mc.tagsEnd(), tagConfig.findTag("$electable", "0")));
    ASSERT_EQUALS(1, std::count(mc.tagsBegin(), mc.tagsEnd(), tagConfig.findTag("$all", "0")));
}

TEST(MemberConfig, ParseHorizonFields) {
    ReplSetTagConfig tagConfig;
    MemberConfig mc(BSON("_id" << 0 << "host"
                               << "h"
                               << "horizons"
                               << BSON("alpha"
                                       << "a.host:43"
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
                                    << BSON("goofyRepeatedHorizonName"
                                            << "a.host:43"
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
                                    << BSON("alpha"
                                            << "duplicatedhostname.example.com:42"
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
                                    << BSON("alpha"
                                            << "uniquehostname.example.com:42"
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
                                    << BSON("alpha"
                                            << "uniquehostname.example.com:43"
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
                                    << BSON("alpha"
                                            << "duplicatedhostname.example.com:42"
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
                                    << "horizons"
                                    << BSON(""
                                            << "example.com:42")),
                         &tagConfig);
        ASSERT_TRUE(false);  // Never should get here
    } catch (const ExceptionFor<ErrorCodes::BadValue>& ex) {
        ASSERT_NOT_EQUALS(ex.toStatus().reason().find("Horizons cannot have empty names"),
                          std::string::npos);
    }
}

TEST(MemberConfig, ValidateVotes) {
    ReplSetTagConfig tagConfig;

    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "votes" << 1.0),
                        &tagConfig);
        ASSERT_OK(mc.validate());
        ASSERT_TRUE(mc.isVoter());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "votes" << 0 << "priority" << 0),
                        &tagConfig);
        ASSERT_OK(mc.validate());
        ASSERT_FALSE(mc.isVoter());
    }
    {
        // For backwards compatibility, truncate 1.X to 1, and 0.X to 0 (and -0.X to 0).
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "votes" << 1.5),
                        &tagConfig);
        ASSERT_OK(mc.validate());
        ASSERT_TRUE(mc.isVoter());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "votes" << 0.5 << "priority" << 0),
                        &tagConfig);
        ASSERT_OK(mc.validate());
        ASSERT_FALSE(mc.isVoter());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "votes" << -0.5 << "priority" << 0),
                        &tagConfig);
        ASSERT_OK(mc.validate());
        ASSERT_FALSE(mc.isVoter());
    }
    {
        // Invalid values
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "votes" << 2),
                        &tagConfig);
        ASSERT_EQUALS(ErrorCodes::BadValue, mc.validate());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "votes" << -1),
                        &tagConfig);
        ASSERT_EQUALS(ErrorCodes::BadValue, mc.validate());
    }
}

TEST(MemberConfig, ValidatePriorityRanges) {
    ReplSetTagConfig tagConfig;
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 0),
                        &tagConfig);
        ASSERT_OK(mc.validate());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 1000),
                        &tagConfig);
        ASSERT_OK(mc.validate());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << -1),
                        &tagConfig);
        ASSERT_EQUALS(ErrorCodes::BadValue, mc.validate());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 1001),
                        &tagConfig);
        ASSERT_EQUALS(ErrorCodes::BadValue, mc.validate());
    }
}

TEST(MemberConfig, ValidateSlaveDelays) {
    ReplSetTagConfig tagConfig;
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 0 << "slaveDelay" << 0),
                        &tagConfig);
        ASSERT_OK(mc.validate());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 0 << "slaveDelay" << 3600 * 10),
                        &tagConfig);
        ASSERT_OK(mc.validate());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 0 << "slaveDelay" << -1),
                        &tagConfig);
        ASSERT_EQUALS(ErrorCodes::BadValue, mc.validate());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 0 << "slaveDelay" << 3600 * 24 * 400),
                        &tagConfig);
        ASSERT_EQUALS(ErrorCodes::BadValue, mc.validate());
    }
}

TEST(MemberConfig, ValidatePriorityAndSlaveDelayRelationship) {
    ReplSetTagConfig tagConfig;
    MemberConfig mc(BSON("_id" << 0 << "host"
                               << "h"
                               << "priority" << 1 << "slaveDelay" << 60),
                    &tagConfig);
    ASSERT_EQUALS(ErrorCodes::BadValue, mc.validate());
}

TEST(MemberConfig, ValidatePriorityAndHiddenRelationship) {
    ReplSetTagConfig tagConfig;
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 1 << "hidden" << true),
                        &tagConfig);
        ASSERT_EQUALS(ErrorCodes::BadValue, mc.validate());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 1 << "hidden" << false),
                        &tagConfig);
        ASSERT_OK(mc.validate());
    }
}

TEST(MemberConfig, ValidatePriorityAndBuildIndexesRelationship) {
    ReplSetTagConfig tagConfig;
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 1 << "buildIndexes" << false),
                        &tagConfig);

        ASSERT_EQUALS(ErrorCodes::BadValue, mc.validate());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "priority" << 1 << "buildIndexes" << true),
                        &tagConfig);
        ASSERT_OK(mc.validate());
    }
}

TEST(MemberConfig, ValidateArbiterVotesRelationship) {
    ReplSetTagConfig tagConfig;
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "votes" << 1 << "arbiterOnly" << true),
                        &tagConfig);
        ASSERT_OK(mc.validate());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "votes" << 0 << "priority" << 0 << "arbiterOnly" << false),
                        &tagConfig);
        ASSERT_OK(mc.validate());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "votes" << 1 << "arbiterOnly" << false),
                        &tagConfig);
        ASSERT_OK(mc.validate());
    }
    {
        MemberConfig mc(BSON("_id" << 0 << "host"
                                   << "h"
                                   << "votes" << 0 << "arbiterOnly" << true),
                        &tagConfig);
        ASSERT_EQUALS(ErrorCodes::BadValue, mc.validate());
    }
}

}  // namespace
}  // namespace repl
}  // namespace mongo
