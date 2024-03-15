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

#include <cstdint>
#include <limits>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/none.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_set_config_checks.h"
#include "mongo/db/repl/repl_set_config_test.h"
#include "mongo/db/server_options.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/safe_num.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {
namespace {

// Creates a bson document representing a replica set config doc with the given members and votes.
BSONObj createConfigDocWithVoters(int members, int voters = ReplSetConfig::kMaxVotingMembers) {
    str::stream configJson;
    configJson << "{_id:'rs0', version:1, protocolVersion:1, members:[";
    for (int i = 0; i < members; ++i) {
        configJson << "{_id:" << i << ", host:'node" << i << "'";
        if (i >= voters) {
            configJson << ", votes:0, priority:0";
        }
        configJson << "}";
        if (i != (members - 1))
            configJson << ",";
    }
    configJson << "]}";
    return fromjson(configJson);
}

// Creates a bson document representing a replica set config doc with the given members and
// arbiters.
BSONObj createConfigDocWithArbiters(int members, int arbiters = 0) {
    str::stream configJson;
    configJson << "{_id:'rs0', version:1, protocolVersion:1, members:[";
    for (int i = 0; i < members; ++i) {
        configJson << "{_id:" << i << ", host:'node" << i << "'";
        if (i < arbiters) {
            configJson << ", arbiterOnly:true";
        }
        configJson << "}";
        if (i != (members - 1))
            configJson << ",";
    }
    configJson << "]}";
    return fromjson(configJson);
}

TEST(ReplSetConfig, ParseMinimalConfigAndCheckDefaults) {
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "term" << 1 << "protocolVersion" << 1
                                  << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345")))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS("rs0", config.getReplSetName());
    ASSERT_EQUALS(1, config.getConfigVersion());
    ASSERT_EQUALS(1, config.getConfigTerm());
    ASSERT_EQUALS(1, config.getNumMembers());
    ASSERT_EQUALS(MemberId(0), config.membersBegin()->getId());
    ASSERT(holds_alternative<int64_t>(config.getDefaultWriteConcern().w));
    ASSERT_EQUALS(1, get<int64_t>(config.getDefaultWriteConcern().w));
    ASSERT_EQUALS(ReplSetConfig::kDefaultHeartbeatInterval, config.getHeartbeatInterval());
    ASSERT_EQUALS(ReplSetConfig::kDefaultHeartbeatTimeoutPeriod,
                  config.getHeartbeatTimeoutPeriod());
    ASSERT_EQUALS(ReplSetConfig::kDefaultElectionTimeoutPeriod, config.getElectionTimeoutPeriod());
    ASSERT_TRUE(config.isChainingAllowed());
    ASSERT_TRUE(config.getWriteConcernMajorityShouldJournal());
    ASSERT_FALSE(config.getConfigServer_deprecated());
    ASSERT_EQUALS(1, config.getProtocolVersion());
    ASSERT_EQUALS(
        ConnectionString::forReplicaSet("rs0", {HostAndPort{"localhost:12345"}}).toString(),
        config.getConnectionString().toString());
}

TEST(ReplSetConfig, ParseLargeConfigAndCheckAccessors) {
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1234 << "term" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 234 << "host"
                                                           << "localhost:12345"
                                                           << "tags"
                                                           << BSON("NYC"
                                                                   << "NY")))
                                  << "protocolVersion" << 1 << "settings"
                                  << BSON("getLastErrorModes"
                                          << BSON("eastCoast" << BSON("NYC" << 1))
                                          << "chainingAllowed" << false << "heartbeatIntervalMillis"
                                          << 5000 << "heartbeatTimeoutSecs" << 120
                                          << "electionTimeoutMillis" << 10))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS("rs0", config.getReplSetName());
    ASSERT_EQUALS(1234, config.getConfigVersion());
    ASSERT_EQUALS(1, config.getConfigTerm());
    ASSERT_EQUALS(1, config.getNumMembers());
    ASSERT_EQUALS(MemberId(234), config.membersBegin()->getId());
    ASSERT_FALSE(config.isChainingAllowed());
    ASSERT_TRUE(config.getWriteConcernMajorityShouldJournal());
    ASSERT_FALSE(config.getConfigServer_deprecated());
    ASSERT_EQUALS(Seconds(5), config.getHeartbeatInterval());
    ASSERT_EQUALS(Seconds(120), config.getHeartbeatTimeoutPeriod());
    ASSERT_EQUALS(Milliseconds(10), config.getElectionTimeoutPeriod());
    ASSERT_EQUALS(1, config.getProtocolVersion());
    ASSERT_EQUALS(
        ConnectionString::forReplicaSet("rs0", {HostAndPort{"localhost:12345"}}).toString(),
        config.getConnectionString().toString());
}

TEST(ReplSetConfig, GetConnectionStringFiltersHiddenNodes) {
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:11111")
                                                << BSON("_id" << 1 << "host"
                                                              << "localhost:22222"
                                                              << "arbiterOnly" << true)
                                                << BSON("_id" << 2 << "host"
                                                              << "localhost:33333"
                                                              << "hidden" << true << "priority"
                                                              << 0)
                                                << BSON("_id" << 3 << "host"
                                                              << "localhost:44444")))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS(ConnectionString::forReplicaSet(
                      "rs0", {HostAndPort{"localhost:11111"}, HostAndPort{"localhost:44444"}})
                      .toString(),
                  config.getConnectionString().toString());
}

TEST(ReplSetConfig, MajorityCalculationThreeVotersNoArbiters) {
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 2 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                           << "h1:1")
                                                << BSON("_id" << 2 << "host"
                                                              << "h2:1")
                                                << BSON("_id" << 3 << "host"
                                                              << "h3:1")
                                                << BSON("_id" << 4 << "host"
                                                              << "h4:1"
                                                              << "votes" << 0 << "priority" << 0)
                                                << BSON("_id" << 5 << "host"
                                                              << "h5:1"
                                                              << "votes" << 0 << "priority"
                                                              << 0)))));
    ASSERT_OK(config.validate());

    ASSERT_EQUALS(2, config.getWriteMajority());
}

TEST(ReplSetConfig, MajorityCalculationNearlyHalfArbiters) {
    RAIIServerParameterControllerForTest controller{"allowMultipleArbiters", true};
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "mySet"
                                  << "version" << 2 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("host"
                                                     << "node1:12345"
                                                     << "_id" << 0)
                                                << BSON("host"
                                                        << "node2:12345"
                                                        << "_id" << 1)
                                                << BSON("host"
                                                        << "node3:12345"
                                                        << "_id" << 2)
                                                << BSON("host"
                                                        << "node4:12345"
                                                        << "_id" << 3 << "arbiterOnly" << true)
                                                << BSON("host"
                                                        << "node5:12345"
                                                        << "_id" << 4 << "arbiterOnly" << true)))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS(3, config.getWriteMajority());
}

TEST(ReplSetConfig, MajorityCalculationEvenNumberOfMembers) {
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "mySet"
                                  << "version" << 2 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("host"
                                                     << "node1:12345"
                                                     << "_id" << 0)
                                                << BSON("host"
                                                        << "node2:12345"
                                                        << "_id" << 1)
                                                << BSON("host"
                                                        << "node3:12345"
                                                        << "_id" << 2)
                                                << BSON("host"
                                                        << "node4:12345"
                                                        << "_id" << 3)))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS(3, config.getWriteMajority());
}

TEST(ReplSetConfig, MajorityCalculationNearlyHalfSecondariesNoVotes) {
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "mySet"
                                  << "version" << 2 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(
                                         BSON("host"
                                              << "node1:12345"
                                              << "_id" << 0)
                                         << BSON("host"
                                                 << "node2:12345"
                                                 << "_id" << 1 << "votes" << 0 << "priority" << 0)
                                         << BSON("host"
                                                 << "node3:12345"
                                                 << "_id" << 2 << "votes" << 0 << "priority" << 0)
                                         << BSON("host"
                                                 << "node4:12345"
                                                 << "_id" << 3)
                                         << BSON("host"
                                                 << "node5:12345"
                                                 << "_id" << 4)))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS(2, config.getWriteMajority());
}

TEST(ReplSetConfig, ParseFailsWithBadOrMissingIdField) {
    // Replica set name must be a string.
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id" << 1 << "version" << 1 << "members"
                                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                           << "localhost:12345")))),
                  ExceptionFor<ErrorCodes::TypeMismatch>);

    // Replica set name must be present.
    ASSERT_THROWS(
        ReplSetConfig::parse(BSON("version" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345")))),
        DBException);
}

TEST(ReplSetConfig, ParseFailsWithBadOrMissingVersionField) {
    // Config version field must be present.
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345")))),
                  DBException);
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version"
                                            << "1"
                                            << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345")))),
                  ExceptionFor<ErrorCodes::TypeMismatch>);

    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1.0 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345")))));
    ASSERT_OK(config.validate());
    ASSERT_THROWS(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 0.0 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345")))),
        DBException);
    ASSERT_THROWS(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version"
                                  << static_cast<long long>(std::numeric_limits<int>::max()) + 1
                                  << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345")))),
        DBException);
}

TEST(ReplSetConfig, ParseFailsWithBadOrMissingTermField) {
    // Absent term field should set a default.
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345")))));
    ASSERT_EQUALS(config.getConfigTerm(), -1);
    // Serializing the config to BSON should omit a term field with value -1.
    ASSERT_FALSE(config.toBSON().hasField(ReplSetConfig::kConfigTermFieldName));
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "term"
                                            << "1"
                                            << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345")))),
                  ExceptionFor<ErrorCodes::TypeMismatch>);

    config = ReplSetConfig::parse(BSON("_id"
                                       << "rs0"
                                       << "version" << 1 << "term" << 1.0 << "protocolVersion" << 1
                                       << "members"
                                       << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                << "localhost:12345"))));
    ASSERT_OK(config.validate());
    config = ReplSetConfig::parse(BSON("_id"
                                       << "rs0"
                                       << "version" << 1 << "term" << 0.0 << "protocolVersion" << 1
                                       << "members"
                                       << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                << "localhost:12345"))));
    ASSERT_OK(config.validate());
    // Config term can be -1.
    config = ReplSetConfig::parse(BSON("_id"
                                       << "rs0"
                                       << "version" << 1 << "term" << -1.0 << "protocolVersion" << 1
                                       << "members"
                                       << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                << "localhost:12345"))));
    ASSERT_OK(config.validate());
    ASSERT_FALSE(config.toBSON().hasField(ReplSetConfig::kConfigTermFieldName));
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "term" << -2.0 << "protocolVersion"
                                            << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345")))),
                  DBException);
    ASSERT_THROWS(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "term"
                                  << static_cast<long long>(std::numeric_limits<int>::max()) + 1
                                  << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345")))),
        DBException);
}

TEST(ReplSetConfig, ParseFailsWithBadMembers) {
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345")
                                                          << "localhost:23456"))),
                  ExceptionFor<ErrorCodes::TypeMismatch>);
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("host"
                                                               << "localhost:12345")))),
                  DBException);
}

TEST(ReplSetConfig, ParseFailsWithLocalNonLocalHostMix) {
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost")
                                                << BSON("_id" << 1 << "host"
                                                              << "otherhost")))));
    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());
}

TEST(ReplSetConfig, ParseFailsWithNoElectableNodes) {
    const BSONObj configBsonNoElectableNodes = BSON("_id"
                                                    << "rs0"
                                                    << "version" << 1 << "protocolVersion" << 1
                                                    << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "localhost:1"
                                                                             << "priority" << 0)
                                                                  << BSON("_id" << 1 << "host"
                                                                                << "localhost:2"
                                                                                << "priority"
                                                                                << 0)));

    ReplSetConfig config(ReplSetConfig::parse(configBsonNoElectableNodes));
    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());

    const BSONObj configBsonNoElectableNodesOneArbiter =
        BSON("_id"
             << "rs0"
             << "version" << 1 << "protocolVersion" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "localhost:1"
                                      << "arbiterOnly" << 1)
                           << BSON("_id" << 1 << "host"
                                         << "localhost:2"
                                         << "priority" << 0)));

    config = ReplSetConfig::parse(configBsonNoElectableNodesOneArbiter);
    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());

    const BSONObj configBsonNoElectableNodesTwoArbiters =
        BSON("_id"
             << "rs0"
             << "version" << 1 << "protocolVersion" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "localhost:1"
                                      << "arbiterOnly" << 1)
                           << BSON("_id" << 1 << "host"
                                         << "localhost:2"
                                         << "arbiterOnly" << 1)));

    config = ReplSetConfig::parse(configBsonNoElectableNodesOneArbiter);
    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());

    const BSONObj configBsonOneElectableNode = BSON("_id"
                                                    << "rs0"
                                                    << "version" << 1 << "protocolVersion" << 1
                                                    << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "localhost:1"
                                                                             << "priority" << 0)
                                                                  << BSON("_id" << 1 << "host"
                                                                                << "localhost:2"
                                                                                << "priority"
                                                                                << 1)));
    config = ReplSetConfig::parse(configBsonOneElectableNode);
    ASSERT_OK(config.validate());
}

TEST(ReplSetConfig, ParseFailsWithTooFewVoters) {
    ReplSetConfig config;
    const BSONObj configBsonNoVoters =
        BSON("_id"
             << "rs0"
             << "version" << 1 << "protocolVersion" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "localhost:1"
                                      << "votes" << 0 << "priority" << 0)
                           << BSON("_id" << 1 << "host"
                                         << "localhost:2"
                                         << "votes" << 0 << "priority" << 0)));

    config = ReplSetConfig::parse(configBsonNoVoters);
    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());

    const BSONObj configBsonOneVoter = BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:1"
                                                                     << "votes" << 0 << "priority"
                                                                     << 0)
                                                          << BSON("_id" << 1 << "host"
                                                                        << "localhost:2"
                                                                        << "votes" << 1)));
    config = ReplSetConfig::parse(configBsonOneVoter);
    ASSERT_OK(config.validate());
}

TEST(ReplSetConfig, ParseFailsWithTooManyVoters) {
    ReplSetConfig config(
        ReplSetConfig::parse(createConfigDocWithVoters(8, ReplSetConfig::kMaxVotingMembers)));
    ASSERT_OK(config.validate());
    config =
        ReplSetConfig::parse(createConfigDocWithVoters(8, ReplSetConfig::kMaxVotingMembers + 1));
    ASSERT_NOT_OK(config.validate());
}

TEST(ReplSetConfig, ParseFailsWithDuplicateHost) {
    ReplSetConfig config;
    const BSONObj configBson = BSON("_id"
                                    << "rs0"
                                    << "version" << 1 << "protocolVersion" << 1 << "members"
                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                             << "localhost:1")
                                                  << BSON("_id" << 1 << "host"
                                                                << "localhost:1")));
    config = ReplSetConfig::parse(configBson);
    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());
}

TEST(ReplSetConfig, ParseFailsWithTooManyNodes) {
    ReplSetConfig config;
    namespace mmb = mutablebson;
    mmb::Document configDoc;
    mmb::Element configDocRoot = configDoc.root();
    ASSERT_OK(configDocRoot.appendString("_id", "rs0"));
    ASSERT_OK(configDocRoot.appendInt("version", 1));
    ASSERT_OK(configDocRoot.appendInt("protocolVersion", 1));
    mmb::Element membersArray = configDoc.makeElementArray("members");
    ASSERT_OK(configDocRoot.pushBack(membersArray));
    for (size_t i = 0; i < ReplSetConfig::kMaxMembers; ++i) {
        mmb::Element memberElement = configDoc.makeElementObject("");
        ASSERT_OK(membersArray.pushBack(memberElement));
        ASSERT_OK(memberElement.appendInt("_id", i));
        ASSERT_OK(
            memberElement.appendString("host", std::string(str::stream() << "localhost" << i + 1)));
        if (i >= ReplSetConfig::kMaxVotingMembers) {
            ASSERT_OK(memberElement.appendInt("votes", 0));
            ASSERT_OK(memberElement.appendInt("priority", 0));
        }
    }
    const BSONObj configBsonMaxNodes = configDoc.getObject();

    mmb::Element memberElement = configDoc.makeElementObject("");
    ASSERT_OK(membersArray.pushBack(memberElement));
    ASSERT_OK(memberElement.appendInt("_id", ReplSetConfig::kMaxMembers));
    ASSERT_OK(memberElement.appendString(
        "host", std::string(str::stream() << "localhost" << ReplSetConfig::kMaxMembers + 1)));
    ASSERT_OK(memberElement.appendInt("votes", 0));
    ASSERT_OK(memberElement.appendInt("priority", 0));
    const BSONObj configBsonTooManyNodes = configDoc.getObject();


    config = ReplSetConfig::parse(configBsonMaxNodes);
    ASSERT_OK(config.validate());
    config = ReplSetConfig::parse(configBsonTooManyNodes);
    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());
}

TEST(ReplSetConfig, ParseFailsWithUnexpectedField) {
    ReplSetConfig config;
    ASSERT_THROWS(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "unexpectedfield"
                                  << "value"
                                  << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345")))),
        DBException);
}

TEST(ReplSetConfig, ParseFailsWithNonArrayMembersField) {
    ReplSetConfig config;
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << "value")),
                  DBException);
}

TEST(ReplSetConfig, ParseFailsWithNonNumericHeartbeatIntervalMillisField) {
    ReplSetConfig config;
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345"))
                                            << "settings"
                                            << BSON("heartbeatIntervalMillis"
                                                    << "no"))),
                  ExceptionFor<ErrorCodes::TypeMismatch>);

    ASSERT_FALSE(config.isInitialized());

    // Uninitialized configuration should return default heartbeat interval.
    ASSERT_EQUALS(ReplSetConfig::kDefaultHeartbeatInterval, config.getHeartbeatInterval());
}

TEST(ReplSetConfig, ParseFailsWithNonNumericElectionTimeoutMillisField) {
    ReplSetConfig config;
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345"))
                                            << "settings"
                                            << BSON("electionTimeoutMillis"
                                                    << "no"))),
                  ExceptionFor<ErrorCodes::TypeMismatch>);
}

TEST(ReplSetConfig, ParseFailsWithNonNumericHeartbeatTimeoutSecsField) {
    ReplSetConfig config;
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345"))
                                            << "settings"
                                            << BSON("heartbeatTimeoutSecs"
                                                    << "no"))),
                  ExceptionFor<ErrorCodes::TypeMismatch>);
}

TEST(ReplSetConfig, ParseFailsWithNonBoolChainingAllowedField) {
    ReplSetConfig config;
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345"))
                                            << "settings"
                                            << BSON("chainingAllowed"
                                                    << "no"))),
                  ExceptionFor<ErrorCodes::TypeMismatch>);
}

TEST(ReplSetConfig, ParseFailsWithNonBoolConfigServerField) {
    ReplSetConfig config;
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345"))
                                            << "configsvr"
                                            << "no")),
                  DBException);
}

TEST(ReplSetConfig, ParseFailsWithNonObjectSettingsField) {
    ReplSetConfig config;
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345"))
                                            << "settings"
                                            << "none")),
                  ExceptionFor<ErrorCodes::TypeMismatch>);
}

TEST(ReplSetConfig, ParseFailsWithGetLastErrorDefaultsFieldUnparseable) {
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345"))
                                            << "settings"
                                            << BSON("getLastErrorDefaults" << BSON("fsync"
                                                                                   << "seven")))),
                  ExceptionFor<ErrorCodes::TypeMismatch>);
}

TEST(ReplSetConfig, ParseFailsWithNonObjectGetLastErrorDefaultsField) {
    ReplSetConfig config;
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345"))
                                            << "settings"
                                            << BSON("getLastErrorDefaults"
                                                    << "no"))),
                  ExceptionFor<ErrorCodes::TypeMismatch>);
}

TEST(ReplSetConfig, ParseFailsWithNonObjectGetLastErrorModesField) {
    ReplSetConfig config;
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345"))
                                            << "settings"
                                            << BSON("getLastErrorModes"
                                                    << "no"))),
                  ExceptionFor<ErrorCodes::TypeMismatch>);
}

TEST(ReplSetConfig, ParseFailsWithDuplicateGetLastErrorModesField) {
    ReplSetConfig config;
    ASSERT_THROWS_CODE(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345"
                                                           << "tags"
                                                           << BSON("tag"
                                                                   << "yes")))
                                  << "settings"
                                  << BSON("getLastErrorModes"
                                          << BSON("one" << BSON("tag" << 1) << "one"
                                                        << BSON("tag" << 1))))),
        DBException,
        51001);
}

TEST(ReplSetConfig, ParseFailsWithNonObjectGetLastErrorModesEntryField) {
    ReplSetConfig config;
    ASSERT_THROWS(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345"
                                                           << "tags"
                                                           << BSON("tag"
                                                                   << "yes")))
                                  << "settings" << BSON("getLastErrorModes" << BSON("one" << 1)))),
        ExceptionFor<ErrorCodes::TypeMismatch>);
}

TEST(ReplSetConfig, ParseFailsWithNonNumericGetLastErrorModesConstraintValue) {
    ReplSetConfig config;
    ASSERT_THROWS(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345"
                                                           << "tags"
                                                           << BSON("tag"
                                                                   << "yes")))
                                  << "settings"
                                  << BSON("getLastErrorModes" << BSON("one" << BSON("tag"
                                                                                    << "no"))))),
        ExceptionFor<ErrorCodes::TypeMismatch>);
}

TEST(ReplSetConfig, ParseFailsWithNegativeGetLastErrorModesConstraintValue) {
    ReplSetConfig config;
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345"
                                                                     << "tags"
                                                                     << BSON("tag"
                                                                             << "yes")))
                                            << "settings"
                                            << BSON("getLastErrorModes"
                                                    << BSON("one" << BSON("tag" << -1))))),
                  ExceptionFor<ErrorCodes::BadValue>);
}

TEST(ReplSetConfig, ParseFailsWithNonExistentGetLastErrorModesConstraintTag) {
    ReplSetConfig config;
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345"
                                                                     << "tags"
                                                                     << BSON("tag"
                                                                             << "yes")))
                                            << "settings"
                                            << BSON("getLastErrorModes"
                                                    << BSON("one" << BSON("tag2" << 1))))),
                  ExceptionFor<ErrorCodes::NoSuchKey>);
}

TEST(ReplSetConfig, ParseFailsWithRepairField) {
    ReplSetConfig config;
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "repaired" << true << "version" << 1
                                            << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345")))),
                  ExceptionFor<ErrorCodes::RepairedReplicaSetNode>);
}

TEST(ReplSetConfig, ParseFailsWithBadProtocolVersion) {
    ReplSetConfig config;
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "protocolVersion" << 3 << "version" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345")
                                                          << BSON("_id" << 1 << "host"
                                                                        << "localhost:54321")))),
                  DBException);
}

TEST(ReplSetConfig, ParseFailsWithProtocolVersion0) {
    ReplSetConfig config;
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "protocolVersion" << 0 << "version" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345")
                                                          << BSON("_id" << 1 << "host"
                                                                        << "localhost:54321")))),
                  DBException);
}

TEST(ReplSetConfig, ValidateFailsWithDuplicateMemberId) {
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345")
                                                << BSON("_id" << 0 << "host"
                                                              << "someoneelse:12345")))));
    auto status = config.validate();
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(ReplSetConfig, InitializeFailsWithInvalidMember) {
    ReplSetConfig config;
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345"
                                                                     << "hidden" << true)))),
                  DBException);
}

TEST(ReplSetConfig, ChainingAllowedField) {
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345"))
                                  << "settings" << BSON("chainingAllowed" << true))));
    ASSERT_OK(config.validate());
    ASSERT_TRUE(config.isChainingAllowed());

    config = ReplSetConfig::parse(BSON("_id"
                                       << "rs0"
                                       << "version" << 1 << "protocolVersion" << 1 << "members"
                                       << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                << "localhost:12345"))
                                       << "settings" << BSON("chainingAllowed" << false)));
    ASSERT_OK(config.validate());
    ASSERT_FALSE(config.isChainingAllowed());
}

TEST(ReplSetConfig, ConfigServerField) {
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "protocolVersion" << 1 << "version" << 1 << "configsvr" << true
                                  << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345")))));
    ASSERT_TRUE(config.getConfigServer_deprecated());
    // When the field is true it should be serialized.
    BSONObj configBSON = config.toBSON();
    ASSERT_TRUE(configBSON.getField("configsvr").isBoolean());
    ASSERT_TRUE(configBSON.getField("configsvr").boolean());

    ReplSetConfig config2;
    config2 = ReplSetConfig::parse(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "protocolVersion" << 1 << "configsvr"
                                        << false << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "localhost:12345"))));
    ASSERT_FALSE(config2.getConfigServer_deprecated());
    // When the field is false it should not be serialized.
    configBSON = config2.toBSON();
    ASSERT_FALSE(configBSON.hasField("configsvr"));
}

TEST(ReplSetConfig, SetNewlyAddedFieldForMemberConfig) {
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                           << "n1:1")
                                                << BSON("_id" << 2 << "host"
                                                              << "n2:1")))));

    // The member should have its 'newlyAdded' field set to false by default.
    ASSERT_FALSE(config.findMemberByID(1)->isNewlyAdded());
    ASSERT_EQ(2, config.getTotalVotingMembers());
    ASSERT_EQ(2, config.getMajorityVoteCount());
    ASSERT_EQ(2, config.getWriteMajority());
    ASSERT_EQ(2, config.getWritableVotingMembersCount());

    {
        auto modeSW = config.findCustomWriteMode("$majority");
        ASSERT(modeSW.isOK());
        auto modeIt = modeSW.getValue().constraintsBegin();
        ASSERT_EQ(modeIt->getMinCount(), 2);
    }

    auto mutableConfig = config.getMutable();
    mutableConfig.addNewlyAddedFieldForMember(MemberId(1));
    ReplSetConfig newConfig(std::move(mutableConfig));

    ASSERT_TRUE(newConfig.findMemberByID(1)->isNewlyAdded());
    ASSERT_EQ(1, newConfig.getTotalVotingMembers());
    ASSERT_EQ(1, newConfig.getMajorityVoteCount());
    ASSERT_EQ(1, newConfig.getWriteMajority());
    ASSERT_EQ(1, newConfig.getWritableVotingMembersCount());

    {
        auto modeSW = newConfig.findCustomWriteMode("$majority");
        ASSERT(modeSW.isOK());
        auto modeIt = modeSW.getValue().constraintsBegin();
        ASSERT_EQ(modeIt->getMinCount(), 1);
    }
}

TEST(ReplSetConfig, RemoveNewlyAddedFieldForMemberConfig) {
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                           << "n1:1"
                                                           << "newlyAdded" << true)
                                                << BSON("_id" << 2 << "host"
                                                              << "n2:1")))));


    ASSERT_TRUE(config.findMemberByID(1)->isNewlyAdded());
    ASSERT_EQ(1, config.getTotalVotingMembers());
    ASSERT_EQ(1, config.getMajorityVoteCount());
    ASSERT_EQ(1, config.getWriteMajority());
    ASSERT_EQ(1, config.getWritableVotingMembersCount());

    {
        auto modeSW = config.findCustomWriteMode("$majority");
        ASSERT(modeSW.isOK());
        auto modeIt = modeSW.getValue().constraintsBegin();
        ASSERT_EQ(modeIt->getMinCount(), 1);
    }

    auto mutableConfig = config.getMutable();
    mutableConfig.removeNewlyAddedFieldForMember(MemberId(1));
    ReplSetConfig newConfig(std::move(mutableConfig));

    ASSERT_FALSE(newConfig.findMemberByID(1)->isNewlyAdded());
    ASSERT_EQ(2, newConfig.getTotalVotingMembers());
    ASSERT_EQ(2, newConfig.getMajorityVoteCount());
    ASSERT_EQ(2, newConfig.getWriteMajority());
    ASSERT_EQ(2, newConfig.getWritableVotingMembersCount());

    {
        auto modeSW = newConfig.findCustomWriteMode("$majority");
        ASSERT(modeSW.isOK());
        auto modeIt = modeSW.getValue().constraintsBegin();
        ASSERT_EQ(modeIt->getMinCount(), 2);
    }
}

TEST(ReplSetConfig, ParsingNewlyAddedSetsFieldToTrueCorrectly) {
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                           << "localhost:12345"
                                                           << "newlyAdded" << true)))));

    // The member should have its 'newlyAdded' field set to true after parsing.
    ASSERT_TRUE(config.findMemberByID(1)->isNewlyAdded());
}

TEST(ReplSetConfig, ParseFailsWithNewlyAddedSetToFalse) {
    ReplSetConfig config;
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                     << "localhost:12345"
                                                                     << "newlyAdded" << false)))),
                  ExceptionFor<ErrorCodes::InvalidReplicaSetConfig>);
}

TEST(ReplSetConfig, NodeWithNewlyAddedFieldHasVotesZero) {
    // Create a config for a three-node set with one arbiter and one node with 'newlyAdded: true'.
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                           << "n1:1"
                                                           << "newlyAdded" << true)
                                                << BSON("_id" << 2 << "host"
                                                              << "n2:1")
                                                << BSON("_id" << 3 << "host"
                                                              << "n3:1"
                                                              << "arbiterOnly" << true)))));

    // Verify that the member had its 'newlyAdded' field set to true after parsing.
    ASSERT_TRUE(config.findMemberByID(1)->isNewlyAdded());
    // Verify that the member is considered a non-voting node.
    ASSERT_FALSE(config.findMemberByID(1)->isVoter());

    // Verify that the rest of the counts were updated correctly.
    ASSERT_EQ(2, config.getTotalVotingMembers());
    ASSERT_EQ(2, config.getMajorityVoteCount());
    ASSERT_EQ(1, config.getWriteMajority());
    ASSERT_EQ(1, config.getWritableVotingMembersCount());
}

TEST(ReplSetConfig, ToBSONWithoutNewlyAdded) {
    // Create a config for a three-node set with one arbiter and one node with 'newlyAdded: true'.
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                           << "n1:1"
                                                           << "newlyAdded" << true)
                                                << BSON("_id" << 2 << "host"
                                                              << "n2:1")
                                                << BSON("_id" << 3 << "host"
                                                              << "n3:1"
                                                              << "arbiterOnly" << true)))));

    // same config, without "newlyAdded: true"
    ReplSetConfig config_expected;

    config_expected =
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                           << "n1:1")
                                                << BSON("_id" << 2 << "host"
                                                              << "n2:1")
                                                << BSON("_id" << 3 << "host"
                                                              << "n3:1"
                                                              << "arbiterOnly" << true))));
    // Sanity check; these objects should not be equal with ordinary serialization, because of the
    // newlyAdded field.
    ASSERT_BSONOBJ_NE(config_expected.toBSON(), config.toBSON());
    ASSERT_BSONOBJ_EQ(config_expected.toBSON(), config.toBSONWithoutNewlyAdded());
    ASSERT_BSONOBJ_EQ(config_expected.toBSONWithoutNewlyAdded(), config.toBSONWithoutNewlyAdded());
}

TEST(ReplSetConfig, ConfigServerFieldDefaults) {
    serverGlobalParams.clusterRole = ClusterRole::None;

    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "protocolVersion" << 1 << "version" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345")))));
    ASSERT_FALSE(config.getConfigServer_deprecated());
    // Default false configsvr field should not be serialized.
    BSONObj configBSON = config.toBSON();
    ASSERT_FALSE(configBSON.hasField("configsvr"));

    ReplSetConfig config2(
        ReplSetConfig::parseForInitiate(BSON("_id"
                                             << "rs0"
                                             << "protocolVersion" << 1 << "version" << 1
                                             << "members"
                                             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                      << "localhost:12345"))),
                                        OID::gen()));
    ASSERT_FALSE(config2.getConfigServer_deprecated());

    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};
    ON_BLOCK_EXIT([&] { serverGlobalParams.clusterRole = ClusterRole::None; });

    ReplSetConfig config3;
    config3 = ReplSetConfig::parse(BSON("_id"
                                        << "rs0"
                                        << "protocolVersion" << 1 << "version" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "localhost:12345"))));
    ASSERT_FALSE(config3.getConfigServer_deprecated());

    ReplSetConfig config4(
        ReplSetConfig::parseForInitiate(BSON("_id"
                                             << "rs0"
                                             << "protocolVersion" << 1 << "version" << 1
                                             << "members"
                                             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                      << "localhost:12345"))),
                                        OID::gen()));
    ASSERT_TRUE(config4.getConfigServer_deprecated());
    // Default true configsvr field should be serialized (even though it wasn't included
    // originally).
    configBSON = config4.toBSON();
    ASSERT_TRUE(configBSON.hasField("configsvr"));
    ASSERT_TRUE(configBSON.getField("configsvr").boolean());
}

TEST(ReplSetConfig, HeartbeatIntervalField) {
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345"))
                                  << "settings" << BSON("heartbeatIntervalMillis" << 5000))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS(Seconds(5), config.getHeartbeatInterval());

    ASSERT_THROWS(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345"))
                                  << "settings" << BSON("heartbeatIntervalMillis" << -5000))),
        DBException);
}

// This test covers the "exact" behavior of all the smallExactInt fields.
TEST(ReplSetConfig, DecimalHeartbeatIntervalField) {
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345"))
                                  << "settings" << BSON("heartbeatIntervalMillis" << 5000.0))));

    ASSERT_THROWS(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345"))
                                  << "settings" << BSON("heartbeatIntervalMillis" << 5000.1))),
        DBException);
}

TEST(ReplSetConfig, ElectionTimeoutField) {
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345"))
                                  << "settings" << BSON("electionTimeoutMillis" << 20))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS(Milliseconds(20), config.getElectionTimeoutPeriod());

    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345"))
                                            << "settings" << BSON("electionTimeoutMillis" << -20))),
                  DBException);
}

TEST(ReplSetConfig, HeartbeatTimeoutField) {
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345"))
                                  << "settings" << BSON("heartbeatTimeoutSecs" << 20))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS(Seconds(20), config.getHeartbeatTimeoutPeriod());

    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345"))
                                            << "settings" << BSON("heartbeatTimeoutSecs" << -20))),
                  DBException);
}

TEST(ReplSetConfig, toBSONRoundTripAbility) {
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
}

TEST(ReplSetConfig, toBSONRoundTripAbilityWithHorizon) {
    ReplSetConfig configA;
    ReplSetConfig configB;
    configA = ReplSetConfig::parse(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "localhost:12345"
                                                                 << "horizons"
                                                                 << BSON("horizon"
                                                                         << "example.com:42")))
                                        << "settings"
                                        << BSON("heartbeatIntervalMillis"
                                                << 5000 << "heartbeatTimeoutSecs" << 20
                                                << "replicaSetId" << OID::gen())));
    configB = ReplSetConfig::parse(configA.toBSON());
    ASSERT_TRUE(configA == configB);
}

TEST(ReplSetConfig, toBSONRoundTripAbilityLarge) {
    ReplSetConfig configA;
    ReplSetConfig configB;
    configA = ReplSetConfig::parse(BSON(
        "_id"
        << "asdf"
        << "version" << 9 << "writeConcernMajorityJournalDefault" << true << "members"
        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                 << "localhost:12345"
                                 << "arbiterOnly" << true << "votes" << 1)
                      << BSON("_id" << 3 << "host"
                                    << "localhost:3828"
                                    << "arbiterOnly" << false << "hidden" << true << "buildIndexes"
                                    << false << "priority" << 0 << "secondaryDelaySecs" << 17
                                    << "votes" << 0 << "newlyAdded" << true << "tags"
                                    << BSON("coast"
                                            << "east"
                                            << "ssd"
                                            << "true"))
                      << BSON("_id" << 2 << "host"
                                    << "foo.com:3828"
                                    << "votes" << 0 << "priority" << 0 << "tags"
                                    << BSON("coast"
                                            << "west"
                                            << "hdd"
                                            << "true")))
        << "protocolVersion" << 1 << "settings"

        << BSON("heartbeatIntervalMillis"
                << 5000 << "heartbeatTimeoutSecs" << 20 << "electionTimeoutMillis" << 4
                << "chainingAllowed" << true << "getLastErrorModes"
                << BSON("disks" << BSON("ssd" << 1 << "hdd" << 1) << "coasts"
                                << BSON("coast" << 2)))));
    BSONObj configObjA = configA.toBSON();
    configB = ReplSetConfig::parse(configObjA);
    ASSERT_TRUE(configA == configB);
}

TEST(ReplSetConfig, toBSONRoundTripAbilityInvalid) {
    ReplSetConfig configA;
    ReplSetConfig configB;
    ASSERT_THROWS(
        ReplSetConfig::parse(BSON(
            "_id"
            << ""
            << "version" << -3 << "protocolVersion" << 1 << "members"
            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                     << "localhost:12345"
                                     << "arbiterOnly" << true << "votes" << 0 << "priority" << 0)
                          << BSON("_id" << 0 << "host"
                                        << "localhost:3828"
                                        << "arbiterOnly" << false << "buildIndexes" << false
                                        << "priority" << 2)
                          << BSON("_id" << 2 << "host"
                                        << "localhost:3828"
                                        << "votes" << 0 << "priority" << 0))
            << "settings"
            << BSON("heartbeatIntervalMillis" << -5000 << "heartbeatTimeoutSecs" << 20
                                              << "electionTimeoutMillis" << 2))),
        DBException);
    configB = ReplSetConfig::parse(configA.toBSON());
    ASSERT_NOT_OK(configA.validate());
    ASSERT_NOT_OK(configB.validate());
    ASSERT_TRUE(configA == configB);
}

TEST(ReplSetConfig, CheckIfWriteConcernCanBeSatisfied) {
    ReplSetConfig configA;
    configA = ReplSetConfig::parse(BSON(
        "_id"
        << "rs0"
        << "version" << 1 << "protocolVersion" << 1 << "members"
        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                 << "node0"
                                 << "tags"
                                 << BSON("dc"
                                         << "NA"
                                         << "rack"
                                         << "rackNA1"))
                      << BSON("_id" << 1 << "host"
                                    << "node1"
                                    << "tags"
                                    << BSON("dc"
                                            << "NA"
                                            << "rack"
                                            << "rackNA2"))
                      << BSON("_id" << 2 << "host"
                                    << "node2"
                                    << "tags"
                                    << BSON("dc"
                                            << "NA"
                                            << "rack"
                                            << "rackNA3"))
                      << BSON("_id" << 3 << "host"
                                    << "node3"
                                    << "tags"
                                    << BSON("dc"
                                            << "EU"
                                            << "rack"
                                            << "rackEU1"))
                      << BSON("_id" << 4 << "host"
                                    << "node4"
                                    << "tags"
                                    << BSON("dc"
                                            << "EU"
                                            << "rack"
                                            << "rackEU2"))
                      << BSON("_id" << 5 << "host"
                                    << "node5"
                                    << "arbiterOnly" << true))
        << "settings"
        << BSON("getLastErrorModes" << BSON(
                    "valid" << BSON("dc" << 2 << "rack" << 3) << "invalidNotEnoughValues"
                            << BSON("dc" << 3) << "invalidNotEnoughNodes" << BSON("rack" << 6)))));

    WriteConcernOptions validNumberWC;
    validNumberWC.w = 5;
    ASSERT_OK(configA.checkIfWriteConcernCanBeSatisfied(validNumberWC));

    WriteConcernOptions invalidNumberWC;
    invalidNumberWC.w = 6;
    ASSERT_EQUALS(ErrorCodes::UnsatisfiableWriteConcern,
                  configA.checkIfWriteConcernCanBeSatisfied(invalidNumberWC));

    WriteConcernOptions majorityWC;
    majorityWC.w = "majority";
    ASSERT_OK(configA.checkIfWriteConcernCanBeSatisfied(majorityWC));

    WriteConcernOptions validModeWC;
    validModeWC.w = "valid";
    ASSERT_OK(configA.checkIfWriteConcernCanBeSatisfied(validModeWC));

    WriteConcernOptions fakeModeWC;
    fakeModeWC.w = "fake";
    ASSERT_EQUALS(ErrorCodes::UnknownReplWriteConcern,
                  configA.checkIfWriteConcernCanBeSatisfied(fakeModeWC));

    WriteConcernOptions invalidModeNotEnoughValuesWC;
    invalidModeNotEnoughValuesWC.w = "invalidNotEnoughValues";
    ASSERT_EQUALS(ErrorCodes::UnsatisfiableWriteConcern,
                  configA.checkIfWriteConcernCanBeSatisfied(invalidModeNotEnoughValuesWC));

    WriteConcernOptions invalidModeNotEnoughNodesWC;
    invalidModeNotEnoughNodesWC.w = "invalidNotEnoughNodes";
    ASSERT_EQUALS(ErrorCodes::UnsatisfiableWriteConcern,
                  configA.checkIfWriteConcernCanBeSatisfied(invalidModeNotEnoughNodesWC));
}

TEST(ReplSetConfig, CheckMaximumNodesOkay) {
    ReplSetConfig configA;
    ReplSetConfig configB;
    const int memberCount = 50;
    configA = ReplSetConfig::parse(createConfigDocWithVoters(memberCount));
    configB = ReplSetConfig::parse(configA.toBSON());
    ASSERT_OK(configA.validate());
    ASSERT_OK(configB.validate());
    ASSERT_TRUE(configA == configB);
}

TEST(ReplSetConfig, CheckBeyondMaximumNodesFailsValidate) {
    ReplSetConfig configA;
    ReplSetConfig configB;
    const int memberCount = 51;
    configA = ReplSetConfig::parse(createConfigDocWithVoters(memberCount));
    configB = ReplSetConfig::parse(configA.toBSON());
    ASSERT_NOT_OK(configA.validate());
    ASSERT_NOT_OK(configB.validate());
    ASSERT_TRUE(configA == configB);
}

TEST(ReplSetConfig, CheckConfigServerCantHaveArbiters) {
    ReplSetConfig configA;
    configA = ReplSetConfig::parse(BSON("_id"
                                        << "rs0"
                                        << "protocolVersion" << 1 << "version" << 1 << "configsvr"
                                        << true << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "localhost:12345")
                                                      << BSON("_id" << 1 << "host"
                                                                    << "localhost:54321"
                                                                    << "arbiterOnly" << true))));
    Status status = configA.validate();
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_STRING_CONTAINS(status.reason(), "Arbiters are not allowed");
}

TEST(ReplSetConfig, CheckConfigServerMustBuildIndexes) {
    ReplSetConfig configA;
    configA = ReplSetConfig::parse(BSON("_id"
                                        << "rs0"
                                        << "protocolVersion" << 1 << "version" << 1 << "configsvr"
                                        << true << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "localhost:12345")
                                                      << BSON("_id" << 1 << "host"
                                                                    << "localhost:54321"
                                                                    << "priority" << 0
                                                                    << "buildIndexes" << false))));
    Status status = configA.validate();
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_STRING_CONTAINS(status.reason(), "must build indexes");
}

TEST(ReplSetConfig, CheckConfigServerCantHaveSecondaryDelaySecs) {
    ReplSetConfig configA;
    configA = ReplSetConfig::parse(
        BSON("_id"
             << "rs0"
             << "protocolVersion" << 1 << "version" << 1 << "configsvr" << true << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "localhost:12345")
                           << BSON("_id" << 1 << "host"
                                         << "localhost:54321"
                                         << "priority" << 0 << "secondaryDelaySecs" << 3))));
    Status status = configA.validate();
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_STRING_CONTAINS(status.reason(), "cannot have a non-zero secondaryDelaySecs");
}

TEST(ReplSetConfig, CheckConfigServerMustHaveTrueForWriteConcernMajorityJournalDefault) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};
    ON_BLOCK_EXIT([&] { serverGlobalParams.clusterRole = ClusterRole::None; });
    ReplSetConfig configA;
    configA = ReplSetConfig::parse(BSON("_id"
                                        << "rs0"
                                        << "protocolVersion" << 1 << "version" << 1 << "configsvr"
                                        << true << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "localhost:12345")
                                                      << BSON("_id" << 1 << "host"
                                                                    << "localhost:54321"))
                                        << "writeConcernMajorityJournalDefault" << false));
    Status status = configA.validate();
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_STRING_CONTAINS(status.reason(), " must be true in replica set configurations being ");
}

TEST(ReplSetConfig, GetPriorityTakeoverDelay) {
    ReplSetConfig configA;
    configA = ReplSetConfig::parse(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "localhost:12345"
                                                                 << "priority" << 1)
                                                      << BSON("_id" << 1 << "host"
                                                                    << "localhost:54321"
                                                                    << "priority" << 2)
                                                      << BSON("_id" << 2 << "host"
                                                                    << "localhost:5321"
                                                                    << "priority" << 3)
                                                      << BSON("_id" << 3 << "host"
                                                                    << "localhost:5421"
                                                                    << "priority" << 4)
                                                      << BSON("_id" << 4 << "host"
                                                                    << "localhost:5431"
                                                                    << "priority" << 5))
                                        << "settings" << BSON("electionTimeoutMillis" << 1000)));
    ASSERT_OK(configA.validate());
    ASSERT_EQUALS(Milliseconds(5000), configA.getPriorityTakeoverDelay(0));
    ASSERT_EQUALS(Milliseconds(4000), configA.getPriorityTakeoverDelay(1));
    ASSERT_EQUALS(Milliseconds(3000), configA.getPriorityTakeoverDelay(2));
    ASSERT_EQUALS(Milliseconds(2000), configA.getPriorityTakeoverDelay(3));
    ASSERT_EQUALS(Milliseconds(1000), configA.getPriorityTakeoverDelay(4));

    ReplSetConfig configB;
    configB = ReplSetConfig::parse(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "localhost:12345"
                                                                 << "priority" << 1)
                                                      << BSON("_id" << 1 << "host"
                                                                    << "localhost:54321"
                                                                    << "priority" << 2)
                                                      << BSON("_id" << 2 << "host"
                                                                    << "localhost:5321"
                                                                    << "priority" << 2)
                                                      << BSON("_id" << 3 << "host"
                                                                    << "localhost:5421"
                                                                    << "priority" << 3)
                                                      << BSON("_id" << 4 << "host"
                                                                    << "localhost:5431"
                                                                    << "priority" << 3))
                                        << "settings" << BSON("electionTimeoutMillis" << 1000)));
    ASSERT_OK(configB.validate());
    ASSERT_EQUALS(Milliseconds(5000), configB.getPriorityTakeoverDelay(0));
    ASSERT_EQUALS(Milliseconds(3000), configB.getPriorityTakeoverDelay(1));
    ASSERT_EQUALS(Milliseconds(3000), configB.getPriorityTakeoverDelay(2));
    ASSERT_EQUALS(Milliseconds(1000), configB.getPriorityTakeoverDelay(3));
    ASSERT_EQUALS(Milliseconds(1000), configB.getPriorityTakeoverDelay(4));
}

TEST(ReplSetConfig, GetCatchUpTakeoverDelay) {
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345"))
                                  << "settings" << BSON("catchUpTakeoverDelayMillis" << 5000))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS(Milliseconds(5000), config.getCatchUpTakeoverDelay());

    ASSERT_THROWS(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345"))
                                  << "settings" << BSON("catchUpTakeoverDelayMillis" << -5000))),
        DBException);
}

TEST(ReplSetConfig, GetCatchUpTakeoverDelayDefault) {
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345"
                                                           << "priority" << 1)
                                                << BSON("_id" << 1 << "host"
                                                              << "localhost:54321"
                                                              << "priority" << 2)
                                                << BSON("_id" << 2 << "host"
                                                              << "localhost:5321"
                                                              << "priority" << 3)))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS(Milliseconds(30000), config.getCatchUpTakeoverDelay());
}

TEST(ReplSetConfig, ConfirmDefaultValuesOfAndAbilityToSetWriteConcernMajorityJournalDefault) {
    ReplSetConfig config;

    // PV1, should default to true.
    config = ReplSetConfig::parse(BSON("_id"
                                       << "rs0"
                                       << "protocolVersion" << 1 << "version" << 1 << "members"
                                       << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                << "localhost:12345"))));
    ASSERT_OK(config.validate());
    ASSERT_TRUE(config.getWriteConcernMajorityShouldJournal());
    ASSERT_TRUE(config.toBSON().hasField("writeConcernMajorityJournalDefault"));

    // Should be able to set it false in PV1.
    config = ReplSetConfig::parse(BSON("_id"
                                       << "rs0"
                                       << "protocolVersion" << 1 << "version" << 1 << "members"
                                       << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                << "localhost:12345"))
                                       << "writeConcernMajorityJournalDefault" << false));
    ASSERT_OK(config.validate());
    ASSERT_FALSE(config.getWriteConcernMajorityShouldJournal());
    ASSERT_TRUE(config.toBSON().hasField("writeConcernMajorityJournalDefault"));
}

TEST(ReplSetConfig, HorizonConsistency) {
    ReplSetConfig config(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "protocolVersion" << 1 << "version" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345"
                                                           << "horizons"
                                                           << BSON("alpha"
                                                                   << "a.host:42"
                                                                   << "beta"
                                                                   << "a.host2:43"
                                                                   << "gamma"
                                                                   << "a.host3:44"))
                                                << BSON("_id" << 1 << "host"
                                                              << "localhost:23456"
                                                              << "horizons"
                                                              << BSON("alpha"
                                                                      << "b.host:42"
                                                                      << "gamma"
                                                                      << "b.host3:44"))
                                                << BSON("_id" << 2 << "host"
                                                              << "localhost:34567"
                                                              << "horizons"
                                                              << BSON("alpha"
                                                                      << "c.host:42"
                                                                      << "beta"
                                                                      << "c.host1:42"
                                                                      << "gamma"
                                                                      << "c.host2:43"
                                                                      << "delta"

                                                                      << "c.host3:44")))
                                  << "writeConcernMajorityJournalDefault" << false)));

    Status status = config.validate();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(status.reason().find("alpha"), std::string::npos);
    ASSERT_EQUALS(status.reason().find("gamma"), std::string::npos);

    ASSERT_NOT_EQUALS(status.reason().find("beta"), std::string::npos);
    ASSERT_NOT_EQUALS(status.reason().find("delta"), std::string::npos);

    // Within-member duplicates are detected by a different piece of code, first,
    // in the member-config code path.
    config = ReplSetConfig::parse(BSON("_id"
                                       << "rs0"
                                       << "protocolVersion" << 1 << "version" << 1 << "members"
                                       << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                << "same1"
                                                                << "horizons"
                                                                << BSON("alpha"
                                                                        << "a.host:44"
                                                                        << "beta"
                                                                        << "a.host2:44"
                                                                        << "gamma"
                                                                        << "a.host3:44"
                                                                        << "delta"
                                                                        << "a.host4:45"))
                                                     << BSON("_id" << 1 << "host"
                                                                   << "localhost:1"
                                                                   << "horizons"
                                                                   << BSON("alpha"
                                                                           << "same1"
                                                                           << "beta"
                                                                           << "b.host2:44"
                                                                           << "gamma"
                                                                           << "b.host3:44"
                                                                           << "delta"
                                                                           << "b.host4:44"))
                                                     << BSON("_id" << 2 << "host"
                                                                   << "localhost:2"
                                                                   << "horizons"
                                                                   << BSON("alpha"
                                                                           << "c.host1:44"
                                                                           << "beta"
                                                                           << "c.host2:44"
                                                                           << "gamma"
                                                                           << "c.host3:44"
                                                                           << "delta"
                                                                           << "same2"))
                                                     << BSON("_id" << 3 << "host"
                                                                   << "localhost:3"
                                                                   << "horizons"
                                                                   << BSON("alpha"
                                                                           << "same2"
                                                                           << "beta"
                                                                           << "d.host2:44"
                                                                           << "gamma"
                                                                           << "d.host3:44"
                                                                           << "delta"
                                                                           << "d.host4:44")))
                                       << "writeConcernMajorityJournalDefault" << false));

    status = config.validate();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(status.reason().find("a.host"), std::string::npos);
    ASSERT_EQUALS(status.reason().find("b.host"), std::string::npos);
    ASSERT_EQUALS(status.reason().find("c.host"), std::string::npos);
    ASSERT_EQUALS(status.reason().find("d.host"), std::string::npos);
    ASSERT_EQUALS(status.reason().find("localhost"), std::string::npos);

    ASSERT_NOT_EQUALS(status.reason().find("same1"), std::string::npos);
    ASSERT_NOT_EQUALS(status.reason().find("same2"), std::string::npos);
}

TEST(ReplSetConfig, ReplSetId) {
    // Uninitialized configuration has no ID.
    ASSERT_FALSE(ReplSetConfig().hasReplicaSetId());

    // Cannot provide replica set ID in configuration document when initialized from
    // replSetInitiate, because it will not match the new one passed in.
    OID newReplSetId = OID::gen();
    ASSERT_THROWS_WITH_CHECK(
        ReplSetConfig::parseForInitiate(BSON("_id"
                                             << "rs0"
                                             << "version" << 1 << "protocolVersion" << 1
                                             << "members"
                                             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                      << "localhost:12345"
                                                                      << "priority" << 1))
                                             << "settings" << BSON("replicaSetId" << OID::gen())),
                                        newReplSetId),
        ExceptionFor<ErrorCodes::InvalidReplicaSetConfig>,
        ([&](const DBException& ex) {
            ASSERT_STRING_CONTAINS(
                ex.what(),
                "replica set configuration cannot contain 'replicaSetId' field when "
                "called from replSetInitiate");
        }));

    // Cannot initiate with an empty ID.
    ASSERT_THROWS(
        ReplSetConfig::parseForInitiate(BSON("_id"
                                             << "rs0"
                                             << "version" << 1 << "protocolVersion" << 1
                                             << "members"
                                             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                      << "localhost:12345"
                                                                      << "priority" << 1))),
                                        OID()),
        DBException);

    // Configuration created by replSetInitiate should use passed-in replica set ID
    ReplSetConfig configInitiate(
        ReplSetConfig::parseForInitiate(BSON("_id"
                                             << "rs0"
                                             << "version" << 1 << "protocolVersion" << 1
                                             << "members"
                                             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                      << "localhost:12345"
                                                                      << "priority" << 1))),
                                        newReplSetId));
    ASSERT_OK(configInitiate.validate());
    ASSERT_TRUE(configInitiate.hasReplicaSetId());
    OID replicaSetId = configInitiate.getReplicaSetId();
    ASSERT_EQ(newReplSetId, replicaSetId);

    // Configuration initialized from local database can contain ID.
    ReplSetConfig configLocal(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345"
                                                           << "priority" << 1))
                                  << "settings" << BSON("replicaSetId" << replicaSetId))));
    ASSERT_OK(configLocal.validate());
    ASSERT_TRUE(configLocal.hasReplicaSetId());
    ASSERT_EQUALS(replicaSetId, configLocal.getReplicaSetId());

    // When reconfiguring, we can provide a default ID if the configuration does not contain one.
    OID defaultReplicaSetId = OID::gen();
    configLocal = ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345"
                                                                     << "priority" << 1))),
                                       boost::none,
                                       defaultReplicaSetId);
    ASSERT_OK(configLocal.validate());
    ASSERT_TRUE(configLocal.hasReplicaSetId());
    ASSERT_EQUALS(defaultReplicaSetId, configLocal.getReplicaSetId());

    // When reconfiguring, we can provide a default ID if the configuration contains a matching one.

    configLocal =
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345"
                                                           << "priority" << 1))
                                  << "settings" << BSON("replicaSetId" << defaultReplicaSetId)),
                             boost::none,
                             defaultReplicaSetId);
    ASSERT_OK(configLocal.validate());
    ASSERT_TRUE(configLocal.hasReplicaSetId());
    ASSERT_EQUALS(defaultReplicaSetId, configLocal.getReplicaSetId());

    // If the default config does not match the one in the BSON, the one passed-on should be used.
    // (note: this will be rejected by validateConfigForReconfig)
    OID bsonReplicaSetId = OID::gen();
    configLocal =
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345"
                                                           << "priority" << 1))
                                  << "settings" << BSON("replicaSetId" << bsonReplicaSetId)),
                             boost::none,
                             defaultReplicaSetId);
    ASSERT_EQ(bsonReplicaSetId, configLocal.getReplicaSetId());

    // 'replicaSetId' field cannot be explicitly null.
    ASSERT_THROWS_WITH_CHECK(
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "localhost:12345"
                                                           << "priority" << 1))
                                  << "settings" << BSON("replicaSetId" << OID()))),
        ExceptionFor<ErrorCodes::BadValue>,
        ([&](const DBException& ex) {
            ASSERT_STRING_CONTAINS(ex.what(), "replicaSetId field value cannot be null");
        }));


    // 'replicaSetId' field must be an OID.
    ASSERT_THROWS(ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "protocolVersion" << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345"
                                                                     << "priority" << 1))
                                            << "settings" << BSON("replicaSetId" << 12345))),
                  ExceptionFor<ErrorCodes::TypeMismatch>);
}

TEST(ReplSetConfig, ConfigVersionAndTermComparison) {
    // Test equality.
    ASSERT_EQ(ConfigVersionAndTerm(1, 1), ConfigVersionAndTerm(1, 1));
    ASSERT_EQ(ConfigVersionAndTerm(1, 2), ConfigVersionAndTerm(1, 2));
    ASSERT_EQ(ConfigVersionAndTerm(2, 2), ConfigVersionAndTerm(2, 2));
    ASSERT_EQ(ConfigVersionAndTerm(1, -1), ConfigVersionAndTerm(1, 1));
    ASSERT_EQ(ConfigVersionAndTerm(1, 1), ConfigVersionAndTerm(1, -1));
    ASSERT_EQ(ConfigVersionAndTerm(1, -1), ConfigVersionAndTerm(1, -1));
    // Test greater/less than or equal to.
    ASSERT_GT(ConfigVersionAndTerm(2, 1), ConfigVersionAndTerm(1, 1));
    ASSERT_GTE(ConfigVersionAndTerm(2, 1), ConfigVersionAndTerm(1, 1));
    ASSERT_GT(ConfigVersionAndTerm(1, 2), ConfigVersionAndTerm(1, 1));
    ASSERT_GTE(ConfigVersionAndTerm(1, 2), ConfigVersionAndTerm(1, 1));
    ASSERT_LT(ConfigVersionAndTerm(1, 1), ConfigVersionAndTerm(2, 1));
    ASSERT_LTE(ConfigVersionAndTerm(1, 1), ConfigVersionAndTerm(2, 1));
    ASSERT_LT(ConfigVersionAndTerm(1, 1), ConfigVersionAndTerm(1, 2));
    ASSERT_LTE(ConfigVersionAndTerm(1, 1), ConfigVersionAndTerm(1, 2));
    ASSERT_GT(ConfigVersionAndTerm(2, 1), ConfigVersionAndTerm(1, -1));
    ASSERT_GT(ConfigVersionAndTerm(2, -1), ConfigVersionAndTerm(1, 1));
    ASSERT_GT(ConfigVersionAndTerm(2, -1), ConfigVersionAndTerm(1, -1));
}
TEST(ReplSetConfig, ConfigVersionAndTermToString) {
    ASSERT_EQ(ConfigVersionAndTerm(0, 1).toString(), "{version: 0, term: 1}");
    ASSERT_EQ(ConfigVersionAndTerm(0, 2).toString(), "{version: 0, term: 2}");
    ASSERT_EQ(ConfigVersionAndTerm(1, 1).toString(), "{version: 1, term: 1}");
    ASSERT_EQ(ConfigVersionAndTerm(1, 2).toString(), "{version: 1, term: 2}");
    ASSERT_EQ(ConfigVersionAndTerm(1, -1).toString(), "{version: 1, term: -1}");
}
TEST(ReplSetConfig, IsImplicitDefaultWriteConcernMajority) {
    RAIIServerParameterControllerForTest controller{"allowMultipleArbiters", true};

    ReplSetConfig config(ReplSetConfig::parse(createConfigDocWithArbiters(1, 0)));
    ASSERT_OK(config.validate());
    ASSERT(config.isImplicitDefaultWriteConcernMajority());

    config = ReplSetConfig::parse(createConfigDocWithArbiters(2, 0));
    ASSERT_OK(config.validate());
    ASSERT(config.isImplicitDefaultWriteConcernMajority());

    config = ReplSetConfig::parse(createConfigDocWithArbiters(3, 0));
    ASSERT_OK(config.validate());
    ASSERT(config.isImplicitDefaultWriteConcernMajority());

    config = ReplSetConfig::parse(createConfigDocWithArbiters(3, 1));
    ASSERT_OK(config.validate());
    ASSERT_FALSE(config.isImplicitDefaultWriteConcernMajority());

    config = ReplSetConfig::parse(createConfigDocWithArbiters(4, 1));
    ASSERT_OK(config.validate());
    ASSERT_FALSE(config.isImplicitDefaultWriteConcernMajority());

    config = ReplSetConfig::parse(createConfigDocWithArbiters(5, 1));
    ASSERT_OK(config.validate());
    ASSERT(config.isImplicitDefaultWriteConcernMajority());

    config = ReplSetConfig::parse(createConfigDocWithArbiters(5, 2));
    ASSERT_OK(config.validate());
    ASSERT_FALSE(config.isImplicitDefaultWriteConcernMajority());
}

TEST(ReplSetConfig, MakeCustomWriteMode) {
    auto config = ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "term" << 1.0 << "protocolVersion"
                                            << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345"
                                                                     << "tags"
                                                                     << BSON("NYC"
                                                                             << "NY")))));

    auto swPattern = config.makeCustomWriteMode({{"NonExistentTag", 1}});
    ASSERT_FALSE(swPattern.isOK());
    ASSERT_EQ(swPattern.getStatus().code(), ErrorCodes::NoSuchKey);

    swPattern = config.makeCustomWriteMode({{"NYC", 1}});
    ASSERT_TRUE(swPattern.isOK());
}

TEST(ReplSetConfig, SameWriteConcernModesNoCustom) {
    auto config = ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "term" << 1.0 << "protocolVersion"
                                            << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345"))));

    auto otherConfig = ReplSetConfig::parse(BSON("_id"
                                                 << "rs0"
                                                 << "version" << 2 << "term" << 1.0
                                                 << "protocolVersion" << 1 << "members"
                                                 << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                          << "localhost:6789"))));

    ASSERT(config.areWriteConcernModesTheSame(&otherConfig));
    ASSERT(otherConfig.areWriteConcernModesTheSame(&config));
}

TEST(ReplSetConfig, SameWriteConcernModesOneCustom) {
    auto config = ReplSetConfig::parse(
        BSON("_id"
             << "rs0"
             << "version" << 1 << "term" << 1.0 << "protocolVersion" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "localhost:12345"
                                      << "tags"
                                      << BSON("NYC"
                                              << "NY")))
             << "settings" << BSON("getLastErrorModes" << BSON("eastCoast" << BSON("NYC" << 1)))));

    auto otherConfig = ReplSetConfig::parse(
        BSON("_id"
             << "rs0"
             << "version" << 2 << "term" << 1.0 << "protocolVersion" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "localhost:6789"
                                      << "tags"
                                      << BSON("NYC"
                                              << "NY")))
             << "settings" << BSON("getLastErrorModes" << BSON("eastCoast" << BSON("NYC" << 1)))));

    ASSERT(config.areWriteConcernModesTheSame(&otherConfig));
    ASSERT(otherConfig.areWriteConcernModesTheSame(&config));
}

TEST(ReplSetConfig, DifferentWriteConcernModesOneCustomDifferentName) {
    auto config = ReplSetConfig::parse(
        BSON("_id"
             << "rs0"
             << "version" << 1 << "term" << 1.0 << "protocolVersion" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "localhost:12345"
                                      << "tags"
                                      << BSON("NYC"
                                              << "NY")))
             << "settings" << BSON("getLastErrorModes" << BSON("somename" << BSON("NYC" << 1)))));

    auto otherConfig = ReplSetConfig::parse(
        BSON("_id"
             << "rs0"
             << "version" << 2 << "term" << 1.0 << "protocolVersion" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "localhost:6789"
                                      << "tags"
                                      << BSON("NYC"
                                              << "NY")))
             << "settings" << BSON("getLastErrorModes" << BSON("othername" << BSON("NYC" << 1)))));

    ASSERT_FALSE(config.areWriteConcernModesTheSame(&otherConfig));
    ASSERT_FALSE(otherConfig.areWriteConcernModesTheSame(&config));
}

TEST(ReplSetConfig, DifferentWriteConcernModesOneCustomDifferentCounts) {
    auto config = ReplSetConfig::parse(
        BSON("_id"
             << "rs0"
             << "version" << 1 << "term" << 1.0 << "protocolVersion" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "localhost:12345"
                                      << "tags"
                                      << BSON("NYC"
                                              << "NY"))
                           << BSON("_id" << 1 << "host"
                                         << "otherhost:12345"
                                         << "tags"
                                         << BSON("NYC"
                                                 << "NY")))
             << "settings" << BSON("getLastErrorModes" << BSON("eastCoast" << BSON("NYC" << 1)))));

    auto otherConfig = ReplSetConfig::parse(
        BSON("_id"
             << "rs0"
             << "version" << 1 << "term" << 1.0 << "protocolVersion" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "localhost:12345"
                                      << "tags"
                                      << BSON("NYC"
                                              << "NY"))
                           << BSON("_id" << 1 << "host"
                                         << "otherhost:12345"
                                         << "tags"
                                         << BSON("NYC"
                                                 << "NY")))
             << "settings" << BSON("getLastErrorModes" << BSON("eastCoast" << BSON("NYC" << 2)))));

    ASSERT_FALSE(config.areWriteConcernModesTheSame(&otherConfig));
    ASSERT_FALSE(otherConfig.areWriteConcernModesTheSame(&config));
}

TEST(ReplSetConfig, DifferentWriteConcernModesExtraTag) {
    auto config = ReplSetConfig::parse(BSON("_id"
                                            << "rs0"
                                            << "version" << 1 << "term" << 1.0 << "protocolVersion"
                                            << 1 << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:12345"
                                                                     << "tags"
                                                                     << BSON("NYC"
                                                                             << "NY"))
                                                          << BSON("_id" << 1 << "host"
                                                                        << "otherhost:12345"
                                                                        << "tags"
                                                                        << BSON("Boston"
                                                                                << "MA")))
                                            << "settings"
                                            << BSON("getLastErrorModes"
                                                    << BSON("nyonly" << BSON("NYC" << 1) << "maonly"
                                                                     << BSON("Boston" << 1)))));

    auto otherConfig = ReplSetConfig::parse(
        BSON("_id"
             << "rs0"
             << "version" << 1 << "term" << 1.0 << "protocolVersion" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "localhost:12345"
                                      << "tags"
                                      << BSON("NYC"
                                              << "NY"))
                           << BSON("_id" << 1 << "host"
                                         << "otherhost:12345"
                                         << "tags"
                                         << BSON("Boston"
                                                 << "MA")))
             << "settings" << BSON("getLastErrorModes" << BSON("nyonly" << BSON("NYC" << 1)))));

    ASSERT_FALSE(config.areWriteConcernModesTheSame(&otherConfig));
    ASSERT_FALSE(otherConfig.areWriteConcernModesTheSame(&config));
}

TEST(ReplSetConfig, DifferentWriteConcernModesSameNameDifferentDefinition) {
    auto config = ReplSetConfig::parse(
        BSON("_id"
             << "rs0"
             << "version" << 1 << "term" << 1.0 << "protocolVersion" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "localhost:12345"
                                      << "tags"
                                      << BSON("NYC"
                                              << "NY"))
                           << BSON("_id" << 1 << "host"
                                         << "otherhost:12345"
                                         << "tags"
                                         << BSON("Boston"
                                                 << "MA")))
             << "settings" << BSON("getLastErrorModes" << BSON("eastCoast" << BSON("NYC" << 1)))));

    auto otherConfig = ReplSetConfig::parse(BSON(
        "_id"
        << "rs0"
        << "version" << 1 << "term" << 1.0 << "protocolVersion" << 1 << "members"
        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                 << "localhost:12345"
                                 << "tags"
                                 << BSON("NYC"
                                         << "NY"))
                      << BSON("_id" << 1 << "host"
                                    << "otherhost:12345"
                                    << "tags"
                                    << BSON("Boston"
                                            << "MA")))
        << "settings" << BSON("getLastErrorModes" << BSON("eastCoast" << BSON("Boston" << 1)))));

    ASSERT_FALSE(config.areWriteConcernModesTheSame(&otherConfig));
    ASSERT_FALSE(otherConfig.areWriteConcernModesTheSame(&config));
}

TEST(ReplSetConfig, MutableCompatibilityForRecipientConfig) {
    const std::string recipientTagName{"recipient"};
    const auto donorReplSetId = OID::gen();
    const std::string recipientConfigSetName{"recipientSetName"};
    BSONObj recipientConfigBSON = BSON(
        "_id" << recipientConfigSetName << "version" << 1 << "protocolVersion" << 1 << "members"
              << BSON_ARRAY(BSON("_id" << 0 << "host"
                                       << "localhost:20002"
                                       << "priority" << 1 << "votes" << 1 << "tags"
                                       << BSON(recipientTagName << "one")))
              << "settings"
              << BSON("heartbeatIntervalMillis" << 5000 << "heartbeatTimeoutSecs" << 20));
    BSONObj replSetConfigWithRecipientConfig = BSON("_id"
                                                    << "rs0"
                                                    << "version" << 1 << "protocolVersion" << 1
                                                    << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "localhost:12345"))
                                                    << "settings"
                                                    << BSON("heartbeatIntervalMillis"
                                                            << 5000 << "heartbeatTimeoutSecs" << 20
                                                            << "replicaSetId" << donorReplSetId)
                                                    << "recipientConfig" << recipientConfigBSON);

    ReplSetConfig config(ReplSetConfig::parse(replSetConfigWithRecipientConfig));
    ASSERT_OK(config.validate());
    auto mutableConfig = config.getMutable();
    mutableConfig.setConfigVersion(1);
    mutableConfig.setConfigTerm(1);
    ReplSetConfig rolledBackConfig = ReplSetConfig(std::move(mutableConfig));
    ASSERT_OK(rolledBackConfig.validate());
    ASSERT_EQUALS("rs0", rolledBackConfig.getReplSetName());
    ASSERT_EQUALS(1, rolledBackConfig.getConfigVersion());
    ASSERT_EQUALS(1, rolledBackConfig.getConfigTerm());
    ASSERT_EQUALS(1, rolledBackConfig.getNumMembers());
    ASSERT_TRUE(sameConfigContents(config, rolledBackConfig));
    ASSERT_FALSE(rolledBackConfig.getRecipientConfig() == nullptr);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
