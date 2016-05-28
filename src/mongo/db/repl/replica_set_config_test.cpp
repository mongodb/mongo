/**
 *    Copyright 2014 MongoDB Inc.
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

#include "mongo/bson/json.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/server_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {
namespace {

// Creates a bson document reprsenting a replica set config doc with the given members, and votes
BSONObj createConfigDoc(int members, int voters = ReplicaSetConfig::kMaxVotingMembers) {
    str::stream configJson;
    configJson << "{_id:'rs0', version:1, members:[";
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

TEST(ReplicaSetConfig, ParseMinimalConfigAndCheckDefaults) {
    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "version"
                                     << 1
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:12345")))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS("rs0", config.getReplSetName());
    ASSERT_EQUALS(1, config.getConfigVersion());
    ASSERT_EQUALS(1, config.getNumMembers());
    ASSERT_EQUALS(0, config.membersBegin()->getId());
    ASSERT_EQUALS(1, config.getDefaultWriteConcern().wNumNodes);
    ASSERT_EQUALS("", config.getDefaultWriteConcern().wMode);
    ASSERT_EQUALS(ReplicaSetConfig::kDefaultHeartbeatInterval, config.getHeartbeatInterval());
    ASSERT_EQUALS(ReplicaSetConfig::kDefaultHeartbeatTimeoutPeriod,
                  config.getHeartbeatTimeoutPeriod());
    ASSERT_EQUALS(ReplicaSetConfig::kDefaultElectionTimeoutPeriod,
                  config.getElectionTimeoutPeriod());
    ASSERT_TRUE(config.isChainingAllowed());
    ASSERT_FALSE(config.getWriteConcernMajorityShouldJournal());
    ASSERT_FALSE(config.isConfigServer());
    ASSERT_EQUALS(0, config.getProtocolVersion());
    ASSERT_EQUALS(
        ConnectionString::forReplicaSet("rs0", {HostAndPort{"localhost:12345"}}).toString(),
        config.getConnectionString().toString());
}

TEST(ReplicaSetConfig, ParseLargeConfigAndCheckAccessors) {
    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "version"
                                     << 1234
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 234 << "host"
                                                              << "localhost:12345"
                                                              << "tags"
                                                              << BSON("NYC"
                                                                      << "NY")))
                                     << "protocolVersion"
                                     << 1
                                     << "settings"
                                     << BSON("getLastErrorDefaults"
                                             << BSON("w"
                                                     << "majority")
                                             << "getLastErrorModes"
                                             << BSON("eastCoast" << BSON("NYC" << 1))
                                             << "chainingAllowed"
                                             << false
                                             << "heartbeatIntervalMillis"
                                             << 5000
                                             << "heartbeatTimeoutSecs"
                                             << 120
                                             << "electionTimeoutMillis"
                                             << 10))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS("rs0", config.getReplSetName());
    ASSERT_EQUALS(1234, config.getConfigVersion());
    ASSERT_EQUALS(1, config.getNumMembers());
    ASSERT_EQUALS(234, config.membersBegin()->getId());
    ASSERT_EQUALS(0, config.getDefaultWriteConcern().wNumNodes);
    ASSERT_EQUALS("majority", config.getDefaultWriteConcern().wMode);
    ASSERT_FALSE(config.isChainingAllowed());
    ASSERT_TRUE(config.getWriteConcernMajorityShouldJournal());
    ASSERT_FALSE(config.isConfigServer());
    ASSERT_EQUALS(Seconds(5), config.getHeartbeatInterval());
    ASSERT_EQUALS(Seconds(120), config.getHeartbeatTimeoutPeriod());
    ASSERT_EQUALS(Milliseconds(10), config.getElectionTimeoutPeriod());
    ASSERT_EQUALS(1, config.getProtocolVersion());
    ASSERT_EQUALS(
        ConnectionString::forReplicaSet("rs0", {HostAndPort{"localhost:12345"}}).toString(),
        config.getConnectionString().toString());
}

TEST(ReplicaSetConfig, GetConnectionStringFiltersHiddenNodes) {
    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "version"
                                     << 1
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:11111")
                                                   << BSON("_id" << 1 << "host"
                                                                 << "localhost:22222"
                                                                 << "arbiterOnly"
                                                                 << true)
                                                   << BSON("_id" << 2 << "host"
                                                                 << "localhost:33333"
                                                                 << "hidden"
                                                                 << true
                                                                 << "priority"
                                                                 << 0)
                                                   << BSON("_id" << 3 << "host"
                                                                 << "localhost:44444")))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS(ConnectionString::forReplicaSet(
                      "rs0", {HostAndPort{"localhost:11111"}, HostAndPort{"localhost:44444"}})
                      .toString(),
                  config.getConnectionString().toString());
}

TEST(ReplicaSetConfig, MajorityCalculationThreeVotersNoArbiters) {
    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "version"
                                     << 2
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                              << "h1:1")
                                                   << BSON("_id" << 2 << "host"
                                                                 << "h2:1")
                                                   << BSON("_id" << 3 << "host"
                                                                 << "h3:1")
                                                   << BSON("_id" << 4 << "host"
                                                                 << "h4:1"
                                                                 << "votes"
                                                                 << 0
                                                                 << "priority"
                                                                 << 0)
                                                   << BSON("_id" << 5 << "host"
                                                                 << "h5:1"
                                                                 << "votes"
                                                                 << 0
                                                                 << "priority"
                                                                 << 0)))));
    ASSERT_OK(config.validate());

    ASSERT_EQUALS(2, config.getWriteMajority());
}

TEST(ReplicaSetConfig, MajorityCalculationNearlyHalfArbiters) {
    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "mySet"
                                     << "version"
                                     << 2
                                     << "members"
                                     << BSON_ARRAY(BSON("host"
                                                        << "node1:12345"
                                                        << "_id"
                                                        << 0)
                                                   << BSON("host"
                                                           << "node2:12345"
                                                           << "_id"
                                                           << 1)
                                                   << BSON("host"
                                                           << "node3:12345"
                                                           << "_id"
                                                           << 2)
                                                   << BSON("host"
                                                           << "node4:12345"
                                                           << "_id"
                                                           << 3
                                                           << "arbiterOnly"
                                                           << true)
                                                   << BSON("host"
                                                           << "node5:12345"
                                                           << "_id"
                                                           << 4
                                                           << "arbiterOnly"
                                                           << true)))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS(3, config.getWriteMajority());
}

TEST(ReplicaSetConfig, MajorityCalculationEvenNumberOfMembers) {
    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "mySet"
                                     << "version"
                                     << 2
                                     << "members"
                                     << BSON_ARRAY(BSON("host"
                                                        << "node1:12345"
                                                        << "_id"
                                                        << 0)
                                                   << BSON("host"
                                                           << "node2:12345"
                                                           << "_id"
                                                           << 1)
                                                   << BSON("host"
                                                           << "node3:12345"
                                                           << "_id"
                                                           << 2)
                                                   << BSON("host"
                                                           << "node4:12345"
                                                           << "_id"
                                                           << 3)))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS(3, config.getWriteMajority());
}

TEST(ReplicaSetConfig, MajorityCalculationNearlyHalfSecondariesNoVotes) {
    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "mySet"
                                     << "version"
                                     << 2
                                     << "members"
                                     << BSON_ARRAY(BSON("host"
                                                        << "node1:12345"
                                                        << "_id"
                                                        << 0)
                                                   << BSON("host"
                                                           << "node2:12345"
                                                           << "_id"
                                                           << 1
                                                           << "votes"
                                                           << 0
                                                           << "priority"
                                                           << 0)
                                                   << BSON("host"
                                                           << "node3:12345"
                                                           << "_id"
                                                           << 2
                                                           << "votes"
                                                           << 0
                                                           << "priority"
                                                           << 0)
                                                   << BSON("host"
                                                           << "node4:12345"
                                                           << "_id"
                                                           << 3)
                                                   << BSON("host"
                                                           << "node5:12345"
                                                           << "_id"
                                                           << 4)))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS(2, config.getWriteMajority());
}

TEST(ReplicaSetConfig, ParseFailsWithBadOrMissingIdField) {
    ReplicaSetConfig config;
    // Replica set name must be a string.
    ASSERT_EQUALS(ErrorCodes::TypeMismatch,
                  config.initialize(BSON("_id" << 1 << "version" << 1 << "members"
                                               << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                        << "localhost:12345")))));

    // Replica set name must be present.
    ASSERT_EQUALS(
        ErrorCodes::NoSuchKey,
        config.initialize(
            BSON("version" << 1 << "members" << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                      << "localhost:12345")))));

    // Empty repl set name parses, but does not validate.
    ASSERT_OK(config.initialize(BSON("_id"
                                     << ""
                                     << "version"
                                     << 1
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:12345")))));

    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());
}

TEST(ReplicaSetConfig, ParseFailsWithBadOrMissingVersionField) {
    ReplicaSetConfig config;
    // Config version field must be present.
    ASSERT_EQUALS(ErrorCodes::NoSuchKey,
                  config.initialize(BSON("_id"
                                         << "rs0"
                                         << "members"
                                         << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                  << "localhost:12345")))));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch,
                  config.initialize(BSON("_id"
                                         << "rs0"
                                         << "version"
                                         << "1"
                                         << "members"
                                         << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                  << "localhost:12345")))));

    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "version"
                                     << 1.0
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:12345")))));
    ASSERT_OK(config.validate());
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "version"
                                     << 0.0
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:12345")))));
    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "version"
                                     << static_cast<long long>(std::numeric_limits<int>::max()) + 1
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:12345")))));
    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());
}

TEST(ReplicaSetConfig, ParseFailsWithBadMembers) {
    ReplicaSetConfig config;
    ASSERT_EQUALS(ErrorCodes::TypeMismatch,
                  config.initialize(BSON("_id"
                                         << "rs0"
                                         << "version"
                                         << 1
                                         << "members"
                                         << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                  << "localhost:12345")
                                                       << "localhost:23456"))));
    ASSERT_NOT_OK(config.initialize(BSON("_id"
                                         << "rs0"
                                         << "version"
                                         << 1
                                         << "members"
                                         << BSON_ARRAY(BSON("host"
                                                            << "localhost:12345")))));
}

TEST(ReplicaSetConfig, ParseFailsWithLocalNonLocalHostMix) {
    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "version"
                                     << 1
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost")
                                                   << BSON("_id" << 1 << "host"
                                                                 << "otherhost")))));
    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());
}

TEST(ReplicaSetConfig, ParseFailsWithNoElectableNodes) {
    ReplicaSetConfig config;
    const BSONObj configBsonNoElectableNodes = BSON("_id"
                                                    << "rs0"
                                                    << "version"
                                                    << 1
                                                    << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "localhost:1"
                                                                             << "priority"
                                                                             << 0)
                                                                  << BSON("_id" << 1 << "host"
                                                                                << "localhost:2"
                                                                                << "priority"
                                                                                << 0)));

    ASSERT_OK(config.initialize(configBsonNoElectableNodes));
    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());

    const BSONObj configBsonNoElectableNodesOneArbiter = BSON("_id"
                                                              << "rs0"
                                                              << "version"
                                                              << 1
                                                              << "members"
                                                              << BSON_ARRAY(
                                                                     BSON("_id" << 0 << "host"
                                                                                << "localhost:1"
                                                                                << "arbiterOnly"
                                                                                << 1)
                                                                     << BSON("_id" << 1 << "host"
                                                                                   << "localhost:2"
                                                                                   << "priority"
                                                                                   << 0)));

    ASSERT_OK(config.initialize(configBsonNoElectableNodesOneArbiter));
    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());

    const BSONObj configBsonNoElectableNodesTwoArbiters = BSON("_id"
                                                               << "rs0"
                                                               << "version"
                                                               << 1
                                                               << "members"
                                                               << BSON_ARRAY(
                                                                      BSON("_id" << 0 << "host"
                                                                                 << "localhost:1"
                                                                                 << "arbiterOnly"
                                                                                 << 1)
                                                                      << BSON("_id" << 1 << "host"
                                                                                    << "localhost:2"
                                                                                    << "arbiterOnly"
                                                                                    << 1)));

    ASSERT_OK(config.initialize(configBsonNoElectableNodesOneArbiter));
    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());

    const BSONObj configBsonOneElectableNode = BSON("_id"
                                                    << "rs0"
                                                    << "version"
                                                    << 1
                                                    << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "localhost:1"
                                                                             << "priority"
                                                                             << 0)
                                                                  << BSON("_id" << 1 << "host"
                                                                                << "localhost:2"
                                                                                << "priority"
                                                                                << 1)));
    ASSERT_OK(config.initialize(configBsonOneElectableNode));
    ASSERT_OK(config.validate());
}

TEST(ReplicaSetConfig, ParseFailsWithTooFewVoters) {
    ReplicaSetConfig config;
    const BSONObj configBsonNoVoters = BSON("_id"
                                            << "rs0"
                                            << "version"
                                            << 1
                                            << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:1"
                                                                     << "votes"
                                                                     << 0
                                                                     << "priority"
                                                                     << 0)
                                                          << BSON("_id" << 1 << "host"
                                                                        << "localhost:2"
                                                                        << "votes"
                                                                        << 0
                                                                        << "priority"
                                                                        << 0)));

    ASSERT_OK(config.initialize(configBsonNoVoters));
    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());

    const BSONObj configBsonOneVoter = BSON("_id"
                                            << "rs0"
                                            << "version"
                                            << 1
                                            << "members"
                                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                     << "localhost:1"
                                                                     << "votes"
                                                                     << 0
                                                                     << "priority"
                                                                     << 0)
                                                          << BSON("_id" << 1 << "host"
                                                                        << "localhost:2"
                                                                        << "votes"
                                                                        << 1)));
    ASSERT_OK(config.initialize(configBsonOneVoter));
    ASSERT_OK(config.validate());
}

TEST(ReplicaSetConfig, ParseFailsWithTooManyVoters) {
    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(createConfigDoc(8, ReplicaSetConfig::kMaxVotingMembers)));
    ASSERT_OK(config.validate());
    ASSERT_OK(config.initialize(createConfigDoc(8, ReplicaSetConfig::kMaxVotingMembers + 1)));
    ASSERT_NOT_OK(config.validate());
}

TEST(ReplicaSetConfig, ParseFailsWithDuplicateHost) {
    ReplicaSetConfig config;
    const BSONObj configBson = BSON("_id"
                                    << "rs0"
                                    << "version"
                                    << 1
                                    << "members"
                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                             << "localhost:1")
                                                  << BSON("_id" << 1 << "host"
                                                                << "localhost:1")));
    ASSERT_OK(config.initialize(configBson));
    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());
}

TEST(ReplicaSetConfig, ParseFailsWithTooManyNodes) {
    ReplicaSetConfig config;
    namespace mmb = mutablebson;
    mmb::Document configDoc;
    mmb::Element configDocRoot = configDoc.root();
    ASSERT_OK(configDocRoot.appendString("_id", "rs0"));
    ASSERT_OK(configDocRoot.appendInt("version", 1));
    mmb::Element membersArray = configDoc.makeElementArray("members");
    ASSERT_OK(configDocRoot.pushBack(membersArray));
    for (size_t i = 0; i < ReplicaSetConfig::kMaxMembers; ++i) {
        mmb::Element memberElement = configDoc.makeElementObject("");
        ASSERT_OK(membersArray.pushBack(memberElement));
        ASSERT_OK(memberElement.appendInt("_id", i));
        ASSERT_OK(
            memberElement.appendString("host", std::string(str::stream() << "localhost" << i + 1)));
        if (i >= ReplicaSetConfig::kMaxVotingMembers) {
            ASSERT_OK(memberElement.appendInt("votes", 0));
            ASSERT_OK(memberElement.appendInt("priority", 0));
        }
    }
    const BSONObj configBsonMaxNodes = configDoc.getObject();

    mmb::Element memberElement = configDoc.makeElementObject("");
    ASSERT_OK(membersArray.pushBack(memberElement));
    ASSERT_OK(memberElement.appendInt("_id", ReplicaSetConfig::kMaxMembers));
    ASSERT_OK(memberElement.appendString(
        "host", std::string(str::stream() << "localhost" << ReplicaSetConfig::kMaxMembers + 1)));
    ASSERT_OK(memberElement.appendInt("votes", 0));
    ASSERT_OK(memberElement.appendInt("priority", 0));
    const BSONObj configBsonTooManyNodes = configDoc.getObject();


    ASSERT_OK(config.initialize(configBsonMaxNodes));
    ASSERT_OK(config.validate());
    ASSERT_OK(config.initialize(configBsonTooManyNodes));
    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());
}

TEST(ReplicaSetConfig, ParseFailsWithUnexpectedField) {
    ReplicaSetConfig config;
    Status status = config.initialize(BSON("_id"
                                           << "rs0"
                                           << "version"
                                           << 1
                                           << "unexpectedfield"
                                           << "value"));
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(ReplicaSetConfig, ParseFailsWithNonArrayMembersField) {
    ReplicaSetConfig config;
    Status status = config.initialize(BSON("_id"
                                           << "rs0"
                                           << "version"
                                           << 1
                                           << "members"
                                           << "value"));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
}

TEST(ReplicaSetConfig, ParseFailsWithNonNumericHeartbeatIntervalMillisField) {
    ReplicaSetConfig config;
    Status status = config.initialize(BSON("_id"
                                           << "rs0"
                                           << "version"
                                           << 1
                                           << "members"
                                           << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                    << "localhost:12345"))
                                           << "settings"
                                           << BSON("heartbeatIntervalMillis"
                                                   << "no")));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);

    ASSERT_FALSE(config.isInitialized());

    // Uninitialized configuration should return default heartbeat interval.
    ASSERT_EQUALS(ReplicaSetConfig::kDefaultHeartbeatInterval, config.getHeartbeatInterval());
}

TEST(ReplicaSetConfig, ParseFailsWithNonNumericElectionTimeoutMillisField) {
    ReplicaSetConfig config;
    Status status = config.initialize(BSON("_id"
                                           << "rs0"
                                           << "version"
                                           << 1
                                           << "members"
                                           << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                    << "localhost:12345"))
                                           << "settings"
                                           << BSON("electionTimeoutMillis"
                                                   << "no")));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
}

TEST(ReplicaSetConfig, ParseFailsWithNonNumericHeartbeatTimeoutSecsField) {
    ReplicaSetConfig config;
    Status status = config.initialize(BSON("_id"
                                           << "rs0"
                                           << "version"
                                           << 1
                                           << "members"
                                           << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                    << "localhost:12345"))
                                           << "settings"
                                           << BSON("heartbeatTimeoutSecs"
                                                   << "no")));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
}

TEST(ReplicaSetConfig, ParseFailsWithNonBoolChainingAllowedField) {
    ReplicaSetConfig config;
    Status status = config.initialize(BSON("_id"
                                           << "rs0"
                                           << "version"
                                           << 1
                                           << "members"
                                           << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                    << "localhost:12345"))
                                           << "settings"
                                           << BSON("chainingAllowed"
                                                   << "no")));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
}

TEST(ReplicaSetConfig, ParseFailsWithNonBoolConfigServerField) {
    ReplicaSetConfig config;
    Status status = config.initialize(BSON("_id"
                                           << "rs0"
                                           << "version"
                                           << 1
                                           << "members"
                                           << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                    << "localhost:12345"))
                                           << "configsvr"
                                           << "no"));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
}

TEST(ReplicaSetConfig, ParseFailsWithNonObjectSettingsField) {
    ReplicaSetConfig config;
    Status status = config.initialize(BSON("_id"
                                           << "rs0"
                                           << "version"
                                           << 1
                                           << "members"
                                           << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                    << "localhost:12345"))
                                           << "settings"
                                           << "none"));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
}

TEST(ReplicaSetConfig, ParseFailsWithGetLastErrorDefaultsFieldUnparseable) {
    ReplicaSetConfig config;
    Status status = config.initialize(BSON("_id"
                                           << "rs0"
                                           << "version"
                                           << 1
                                           << "members"
                                           << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                    << "localhost:12345"))
                                           << "settings"
                                           << BSON("getLastErrorDefaults" << BSON("fsync"
                                                                                  << "seven"))));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status);
}

TEST(ReplicaSetConfig, ParseFailsWithNonObjectGetLastErrorDefaultsField) {
    ReplicaSetConfig config;
    Status status = config.initialize(BSON("_id"
                                           << "rs0"
                                           << "version"
                                           << 1
                                           << "members"
                                           << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                    << "localhost:12345"))
                                           << "settings"
                                           << BSON("getLastErrorDefaults"
                                                   << "no")));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
}

TEST(ReplicaSetConfig, ParseFailsWithNonObjectGetLastErrorModesField) {
    ReplicaSetConfig config;
    Status status = config.initialize(BSON("_id"
                                           << "rs0"
                                           << "version"
                                           << 1
                                           << "members"
                                           << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                    << "localhost:12345"))
                                           << "settings"
                                           << BSON("getLastErrorModes"
                                                   << "no")));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
}

TEST(ReplicaSetConfig, ParseFailsWithDuplicateGetLastErrorModesField) {
    ReplicaSetConfig config;
    Status status = config.initialize(BSON("_id"
                                           << "rs0"
                                           << "version"
                                           << 1
                                           << "members"
                                           << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                    << "localhost:12345"
                                                                    << "tags"
                                                                    << BSON("tag"
                                                                            << "yes")))
                                           << "settings"
                                           << BSON("getLastErrorModes"
                                                   << BSON("one" << BSON("tag" << 1) << "one"
                                                                 << BSON("tag" << 1)))));
    ASSERT_EQUALS(ErrorCodes::DuplicateKey, status);
}

TEST(ReplicaSetConfig, ParseFailsWithNonObjectGetLastErrorModesEntryField) {
    ReplicaSetConfig config;
    Status status = config.initialize(BSON("_id"
                                           << "rs0"
                                           << "version"
                                           << 1
                                           << "members"
                                           << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                    << "localhost:12345"
                                                                    << "tags"
                                                                    << BSON("tag"
                                                                            << "yes")))
                                           << "settings"
                                           << BSON("getLastErrorModes" << BSON("one" << 1))));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
}

TEST(ReplicaSetConfig, ParseFailsWithNonNumericGetLastErrorModesConstraintValue) {
    ReplicaSetConfig config;
    Status status =
        config.initialize(BSON("_id"
                               << "rs0"
                               << "version"
                               << 1
                               << "members"
                               << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                        << "localhost:12345"
                                                        << "tags"
                                                        << BSON("tag"
                                                                << "yes")))
                               << "settings"
                               << BSON("getLastErrorModes" << BSON("one" << BSON("tag"
                                                                                 << "no")))));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
}

TEST(ReplicaSetConfig, ParseFailsWithNegativeGetLastErrorModesConstraintValue) {
    ReplicaSetConfig config;
    Status status =
        config.initialize(BSON("_id"
                               << "rs0"
                               << "version"
                               << 1
                               << "members"
                               << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                        << "localhost:12345"
                                                        << "tags"
                                                        << BSON("tag"
                                                                << "yes")))
                               << "settings"
                               << BSON("getLastErrorModes" << BSON("one" << BSON("tag" << -1)))));
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(ReplicaSetConfig, ParseFailsWithNonExistentGetLastErrorModesConstraintTag) {
    ReplicaSetConfig config;
    Status status =
        config.initialize(BSON("_id"
                               << "rs0"
                               << "version"
                               << 1
                               << "members"
                               << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                        << "localhost:12345"
                                                        << "tags"
                                                        << BSON("tag"
                                                                << "yes")))
                               << "settings"
                               << BSON("getLastErrorModes" << BSON("one" << BSON("tag2" << 1)))));
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, status);
}

TEST(ReplicaSetConfig, ValidateFailsWithBadProtocolVersion) {
    ReplicaSetConfig config;
    Status status = config.initialize(BSON("_id"
                                           << "rs0"
                                           << "protocolVersion"
                                           << 3
                                           << "version"
                                           << 1
                                           << "members"
                                           << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                    << "localhost:12345")
                                                         << BSON("_id" << 1 << "host"
                                                                       << "localhost:54321"))));
    ASSERT_OK(status);

    status = config.validate();
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(ReplicaSetConfig, ValidateFailsWithDuplicateMemberId) {
    ReplicaSetConfig config;
    Status status = config.initialize(BSON("_id"
                                           << "rs0"
                                           << "version"
                                           << 1
                                           << "members"
                                           << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                    << "localhost:12345")
                                                         << BSON("_id" << 0 << "host"
                                                                       << "someoneelse:12345"))));
    ASSERT_OK(status);

    status = config.validate();
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(ReplicaSetConfig, ValidateFailsWithInvalidMember) {
    ReplicaSetConfig config;
    Status status = config.initialize(BSON("_id"
                                           << "rs0"
                                           << "version"
                                           << 1
                                           << "members"
                                           << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                    << "localhost:12345"
                                                                    << "hidden"
                                                                    << true))));
    ASSERT_OK(status);

    status = config.validate();
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(ReplicaSetConfig, ChainingAllowedField) {
    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "version"
                                     << 1
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:12345"))
                                     << "settings"
                                     << BSON("chainingAllowed" << true))));
    ASSERT_OK(config.validate());
    ASSERT_TRUE(config.isChainingAllowed());

    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "version"
                                     << 1
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:12345"))
                                     << "settings"
                                     << BSON("chainingAllowed" << false))));
    ASSERT_OK(config.validate());
    ASSERT_FALSE(config.isChainingAllowed());
}

TEST(ReplicaSetConfig, ConfigServerField) {
    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "protocolVersion"
                                     << 1
                                     << "version"
                                     << 1
                                     << "configsvr"
                                     << true
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:12345")))));
    ASSERT_TRUE(config.isConfigServer());

    ReplicaSetConfig config2;
    ASSERT_OK(config2.initialize(BSON("_id"
                                      << "rs0"
                                      << "version"
                                      << 1
                                      << "configsvr"
                                      << false
                                      << "members"
                                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                               << "localhost:12345")))));
    ASSERT_FALSE(config2.isConfigServer());

    // Configs in which configsvr is not the same as the --configsvr flag are invalid.
    serverGlobalParams.clusterRole = ClusterRole::ConfigServer;
    ON_BLOCK_EXIT([&] { serverGlobalParams.clusterRole = ClusterRole::None; });

    ASSERT_OK(config.validate());
    ASSERT_EQUALS(ErrorCodes::BadValue, config2.validate());

    serverGlobalParams.clusterRole = ClusterRole::None;
    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());
    ASSERT_OK(config2.validate());
}

TEST(ReplicaSetConfig, ConfigServerFieldDefaults) {
    serverGlobalParams.clusterRole = ClusterRole::None;

    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "protocolVersion"
                                     << 1
                                     << "version"
                                     << 1
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:12345")))));
    ASSERT_FALSE(config.isConfigServer());

    ReplicaSetConfig config2;
    ASSERT_OK(config2.initializeForInitiate(BSON("_id"
                                                 << "rs0"
                                                 << "protocolVersion"
                                                 << 1
                                                 << "version"
                                                 << 1
                                                 << "members"
                                                 << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                          << "localhost:12345")))));
    ASSERT_FALSE(config2.isConfigServer());

    serverGlobalParams.clusterRole = ClusterRole::ConfigServer;
    ON_BLOCK_EXIT([&] { serverGlobalParams.clusterRole = ClusterRole::None; });

    ReplicaSetConfig config3;
    ASSERT_OK(config3.initialize(BSON("_id"
                                      << "rs0"
                                      << "protocolVersion"
                                      << 1
                                      << "version"
                                      << 1
                                      << "members"
                                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                               << "localhost:12345")))));
    ASSERT_FALSE(config3.isConfigServer());

    ReplicaSetConfig config4;
    ASSERT_OK(config4.initializeForInitiate(BSON("_id"
                                                 << "rs0"
                                                 << "protocolVersion"
                                                 << 1
                                                 << "version"
                                                 << 1
                                                 << "members"
                                                 << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                          << "localhost:12345")))));
    ASSERT_TRUE(config4.isConfigServer());
}

TEST(ReplicaSetConfig, HeartbeatIntervalField) {
    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "version"
                                     << 1
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:12345"))
                                     << "settings"
                                     << BSON("heartbeatIntervalMillis" << 5000))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS(Seconds(5), config.getHeartbeatInterval());

    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "version"
                                     << 1
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:12345"))
                                     << "settings"
                                     << BSON("heartbeatIntervalMillis" << -5000))));
    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());
}

TEST(ReplicaSetConfig, ElectionTimeoutField) {
    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "version"
                                     << 1
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:12345"))
                                     << "settings"
                                     << BSON("electionTimeoutMillis" << 20))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS(Milliseconds(20), config.getElectionTimeoutPeriod());

    auto status = config.initialize(BSON("_id"
                                         << "rs0"
                                         << "version"
                                         << 1
                                         << "members"
                                         << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                  << "localhost:12345"))
                                         << "settings"
                                         << BSON("electionTimeoutMillis" << -20)));
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_STRING_CONTAINS(status.reason(), "election timeout must be greater than 0");
}

TEST(ReplicaSetConfig, HeartbeatTimeoutField) {
    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "version"
                                     << 1
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:12345"))
                                     << "settings"
                                     << BSON("heartbeatTimeoutSecs" << 20))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS(Seconds(20), config.getHeartbeatTimeoutPeriod());

    auto status = config.initialize(BSON("_id"
                                         << "rs0"
                                         << "version"
                                         << 1
                                         << "members"
                                         << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                  << "localhost:12345"))
                                         << "settings"
                                         << BSON("heartbeatTimeoutSecs" << -20)));
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_STRING_CONTAINS(status.reason(), "heartbeat timeout must be greater than 0");
}

TEST(ReplicaSetConfig, GleDefaultField) {
    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "version"
                                     << 1
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:12345"))
                                     << "settings"
                                     << BSON("getLastErrorDefaults" << BSON("w"
                                                                            << "majority")))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS("majority", config.getDefaultWriteConcern().wMode);

    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "version"
                                     << 1
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:12345"))
                                     << "settings"
                                     << BSON("getLastErrorDefaults" << BSON("w"
                                                                            << "frim")))));
    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());

    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "version"
                                     << 1
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:12345"))
                                     << "settings"
                                     << BSON("getLastErrorDefaults" << BSON("w" << 0)))));
    ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());

    ASSERT_OK(
        config.initialize(BSON("_id"
                               << "rs0"
                               << "version"
                               << 1
                               << "members"
                               << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                        << "localhost:12345"
                                                        << "tags"
                                                        << BSON("a"
                                                                << "v")))
                               << "settings"
                               << BSON("getLastErrorDefaults" << BSON("w"
                                                                      << "frim")
                                                              << "getLastErrorModes"
                                                              << BSON("frim" << BSON("a" << 1))))));
    ASSERT_OK(config.validate());
    ASSERT_EQUALS("frim", config.getDefaultWriteConcern().wMode);
    ASSERT_OK(config.findCustomWriteMode("frim").getStatus());
}

bool operator==(const MemberConfig& a, const MemberConfig& b) {
    // do tag comparisons
    for (MemberConfig::TagIterator itrA = a.tagsBegin(); itrA != a.tagsEnd(); ++itrA) {
        if (std::find(b.tagsBegin(), b.tagsEnd(), *itrA) == b.tagsEnd()) {
            return false;
        }
    }
    return a.getId() == b.getId() && a.getHostAndPort() == b.getHostAndPort() &&
        a.getPriority() == b.getPriority() && a.getSlaveDelay() == b.getSlaveDelay() &&
        a.isVoter() == b.isVoter() && a.isArbiter() == b.isArbiter() &&
        a.isHidden() == b.isHidden() && a.shouldBuildIndexes() == b.shouldBuildIndexes() &&
        a.getNumTags() == b.getNumTags();
}

bool operator==(const ReplicaSetConfig& a, const ReplicaSetConfig& b) {
    // compare WriteConcernModes
    std::vector<std::string> modeNames = a.getWriteConcernNames();
    for (std::vector<std::string>::iterator it = modeNames.begin(); it != modeNames.end(); it++) {
        ReplicaSetTagPattern patternA = a.findCustomWriteMode(*it).getValue();
        ReplicaSetTagPattern patternB = b.findCustomWriteMode(*it).getValue();
        for (ReplicaSetTagPattern::ConstraintIterator itrA = patternA.constraintsBegin();
             itrA != patternA.constraintsEnd();
             itrA++) {
            bool same = false;
            for (ReplicaSetTagPattern::ConstraintIterator itrB = patternB.constraintsBegin();
                 itrB != patternB.constraintsEnd();
                 itrB++) {
                if (itrA->getKeyIndex() == itrB->getKeyIndex() &&
                    itrA->getMinCount() == itrB->getMinCount()) {
                    same = true;
                    break;
                }
            }
            if (!same) {
                return false;
            }
        }
    }

    // compare the members
    for (ReplicaSetConfig::MemberIterator memA = a.membersBegin(); memA != a.membersEnd(); memA++) {
        bool same = false;
        for (ReplicaSetConfig::MemberIterator memB = b.membersBegin(); memB != b.membersEnd();
             memB++) {
            if (*memA == *memB) {
                same = true;
                break;
            }
        }
        if (!same) {
            return false;
        }
    }

    // simple comparisons
    return a.getReplSetName() == b.getReplSetName() &&
        a.getConfigVersion() == b.getConfigVersion() && a.getNumMembers() == b.getNumMembers() &&
        a.getHeartbeatInterval() == b.getHeartbeatInterval() &&
        a.getHeartbeatTimeoutPeriod() == b.getHeartbeatTimeoutPeriod() &&
        a.getElectionTimeoutPeriod() == b.getElectionTimeoutPeriod() &&
        a.isChainingAllowed() == b.isChainingAllowed() &&
        a.isConfigServer() == b.isConfigServer() &&
        a.getDefaultWriteConcern().wNumNodes == b.getDefaultWriteConcern().wNumNodes &&
        a.getDefaultWriteConcern().wMode == b.getDefaultWriteConcern().wMode &&
        a.getProtocolVersion() == b.getProtocolVersion() &&
        a.getReplicaSetId() == b.getReplicaSetId();
}

TEST(ReplicaSetConfig, toBSONRoundTripAbility) {
    ReplicaSetConfig configA;
    ReplicaSetConfig configB;
    ASSERT_OK(configA.initialize(BSON(
        "_id"
        << "rs0"
        << "version"
        << 1
        << "members"
        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                 << "localhost:12345"))
        << "settings"
        << BSON("heartbeatIntervalMillis" << 5000 << "heartbeatTimeoutSecs" << 20 << "replicaSetId"
                                          << OID::gen()))));
    ASSERT_OK(configB.initialize(configA.toBSON()));
    ASSERT_TRUE(configA == configB);
}

TEST(ReplicaSetConfig, toBSONRoundTripAbilityLarge) {
    ReplicaSetConfig configA;
    ReplicaSetConfig configB;
    ASSERT_OK(configA.initialize(
        BSON("_id"
             << "asdf"
             << "version"
             << 9
             << "writeConcernMajorityJournalDefault"
             << true
             << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "localhost:12345"
                                      << "arbiterOnly"
                                      << true
                                      << "votes"
                                      << 1)
                           << BSON("_id" << 3 << "host"
                                         << "localhost:3828"
                                         << "arbiterOnly"
                                         << false
                                         << "hidden"
                                         << true
                                         << "buildIndexes"
                                         << false
                                         << "priority"
                                         << 0
                                         << "slaveDelay"
                                         << 17
                                         << "votes"
                                         << 0
                                         << "tags"
                                         << BSON("coast"
                                                 << "east"
                                                 << "ssd"
                                                 << "true"))
                           << BSON("_id" << 2 << "host"
                                         << "foo.com:3828"
                                         << "votes"
                                         << 0
                                         << "priority"
                                         << 0
                                         << "tags"
                                         << BSON("coast"
                                                 << "west"
                                                 << "hdd"
                                                 << "true")))
             << "protocolVersion"
             << 0
             << "settings"

             << BSON("heartbeatIntervalMillis" << 5000 << "heartbeatTimeoutSecs" << 20
                                               << "electionTimeoutMillis"
                                               << 4
                                               << "chainingAllowd"
                                               << true
                                               << "getLastErrorDefaults"
                                               << BSON("w"
                                                       << "majority")
                                               << "getLastErrorModes"
                                               << BSON("disks" << BSON("ssd" << 1 << "hdd" << 1)
                                                               << "coasts"
                                                               << BSON("coast" << 2))))));
    BSONObj configObjA = configA.toBSON();
    // Ensure a protocolVersion does not show up if it is 0 to maintain cross version compatibility.
    ASSERT_FALSE(configObjA.hasField("protocolVersion"));
    ASSERT_OK(configB.initialize(configObjA));
    ASSERT_TRUE(configA == configB);
}

TEST(ReplicaSetConfig, toBSONRoundTripAbilityInvalid) {
    ReplicaSetConfig configA;
    ReplicaSetConfig configB;
    ASSERT_OK(
        configA.initialize(BSON("_id"
                                << ""
                                << "version"
                                << -3
                                << "members"
                                << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                         << "localhost:12345"
                                                         << "arbiterOnly"
                                                         << true
                                                         << "votes"
                                                         << 0
                                                         << "priority"
                                                         << 0)
                                              << BSON("_id" << 0 << "host"
                                                            << "localhost:3828"
                                                            << "arbiterOnly"
                                                            << false
                                                            << "buildIndexes"
                                                            << false
                                                            << "priority"
                                                            << 2)
                                              << BSON("_id" << 2 << "host"
                                                            << "localhost:3828"
                                                            << "votes"
                                                            << 0
                                                            << "priority"
                                                            << 0))
                                << "settings"
                                << BSON("heartbeatIntervalMillis" << -5000 << "heartbeatTimeoutSecs"
                                                                  << 20
                                                                  << "electionTimeoutMillis"
                                                                  << 2))));
    ASSERT_OK(configB.initialize(configA.toBSON()));
    ASSERT_NOT_OK(configA.validate());
    ASSERT_NOT_OK(configB.validate());
    ASSERT_TRUE(configA == configB);
}

TEST(ReplicaSetConfig, CheckIfWriteConcernCanBeSatisfied) {
    ReplicaSetConfig configA;
    ASSERT_OK(configA.initialize(BSON("_id"
                                      << "rs0"
                                      << "version"
                                      << 1
                                      << "members"
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
                                                                  << "arbiterOnly"
                                                                  << true))
                                      << "settings"
                                      << BSON("getLastErrorModes"
                                              << BSON("valid" << BSON("dc" << 2 << "rack" << 3)
                                                              << "invalidNotEnoughValues"
                                                              << BSON("dc" << 3)
                                                              << "invalidNotEnoughNodes"
                                                              << BSON("rack" << 6))))));

    WriteConcernOptions validNumberWC;
    validNumberWC.wNumNodes = 5;
    ASSERT_OK(configA.checkIfWriteConcernCanBeSatisfied(validNumberWC));

    WriteConcernOptions invalidNumberWC;
    invalidNumberWC.wNumNodes = 6;
    ASSERT_EQUALS(ErrorCodes::CannotSatisfyWriteConcern,
                  configA.checkIfWriteConcernCanBeSatisfied(invalidNumberWC));

    WriteConcernOptions majorityWC;
    majorityWC.wMode = "majority";
    ASSERT_OK(configA.checkIfWriteConcernCanBeSatisfied(majorityWC));

    WriteConcernOptions validModeWC;
    validModeWC.wMode = "valid";
    ASSERT_OK(configA.checkIfWriteConcernCanBeSatisfied(validModeWC));

    WriteConcernOptions fakeModeWC;
    fakeModeWC.wMode = "fake";
    ASSERT_EQUALS(ErrorCodes::UnknownReplWriteConcern,
                  configA.checkIfWriteConcernCanBeSatisfied(fakeModeWC));

    WriteConcernOptions invalidModeNotEnoughValuesWC;
    invalidModeNotEnoughValuesWC.wMode = "invalidNotEnoughValues";
    ASSERT_EQUALS(ErrorCodes::CannotSatisfyWriteConcern,
                  configA.checkIfWriteConcernCanBeSatisfied(invalidModeNotEnoughValuesWC));

    WriteConcernOptions invalidModeNotEnoughNodesWC;
    invalidModeNotEnoughNodesWC.wMode = "invalidNotEnoughNodes";
    ASSERT_EQUALS(ErrorCodes::CannotSatisfyWriteConcern,
                  configA.checkIfWriteConcernCanBeSatisfied(invalidModeNotEnoughNodesWC));
}

TEST(ReplicaSetConfig, CheckMaximumNodesOkay) {
    ReplicaSetConfig configA;
    ReplicaSetConfig configB;
    const int memberCount = 50;
    ASSERT_OK(configA.initialize(createConfigDoc(memberCount)));
    ASSERT_OK(configB.initialize(configA.toBSON()));
    ASSERT_OK(configA.validate());
    ASSERT_OK(configB.validate());
    ASSERT_TRUE(configA == configB);
}

TEST(ReplicaSetConfig, CheckBeyondMaximumNodesFailsValidate) {
    ReplicaSetConfig configA;
    ReplicaSetConfig configB;
    const int memberCount = 51;
    ASSERT_OK(configA.initialize(createConfigDoc(memberCount)));
    ASSERT_OK(configB.initialize(configA.toBSON()));
    ASSERT_NOT_OK(configA.validate());
    ASSERT_NOT_OK(configB.validate());
    ASSERT_TRUE(configA == configB);
}

TEST(ReplicaSetConfig, CheckConfigServerCantBeProtocolVersion0) {
    ReplicaSetConfig configA;
    ASSERT_OK(configA.initialize(BSON("_id"
                                      << "rs0"
                                      << "protocolVersion"
                                      << 0
                                      << "version"
                                      << 1
                                      << "configsvr"
                                      << true
                                      << "members"
                                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                               << "localhost:12345")
                                                    << BSON("_id" << 1 << "host"
                                                                  << "localhost:54321"
                                                                  << "arbiterOnly"
                                                                  << true)))));
    Status status = configA.validate();
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_STRING_CONTAINS(status.reason(), "cannot run in protocolVersion 0");
}

TEST(ReplicaSetConfig, CheckConfigServerCantHaveArbiters) {
    ReplicaSetConfig configA;
    ASSERT_OK(configA.initialize(BSON("_id"
                                      << "rs0"
                                      << "protocolVersion"
                                      << 1
                                      << "version"
                                      << 1
                                      << "configsvr"
                                      << true
                                      << "members"
                                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                               << "localhost:12345")
                                                    << BSON("_id" << 1 << "host"
                                                                  << "localhost:54321"
                                                                  << "arbiterOnly"
                                                                  << true)))));
    Status status = configA.validate();
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_STRING_CONTAINS(status.reason(), "Arbiters are not allowed");
}

TEST(ReplicaSetConfig, CheckConfigServerMustBuildIndexes) {
    ReplicaSetConfig configA;
    ASSERT_OK(configA.initialize(BSON("_id"
                                      << "rs0"
                                      << "protocolVersion"
                                      << 1
                                      << "version"
                                      << 1
                                      << "configsvr"
                                      << true
                                      << "members"
                                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                               << "localhost:12345")
                                                    << BSON("_id" << 1 << "host"
                                                                  << "localhost:54321"
                                                                  << "priority"
                                                                  << 0
                                                                  << "buildIndexes"
                                                                  << false)))));
    Status status = configA.validate();
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_STRING_CONTAINS(status.reason(), "must build indexes");
}

TEST(ReplicaSetConfig, CheckConfigServerCantHaveSlaveDelay) {
    ReplicaSetConfig configA;
    ASSERT_OK(configA.initialize(BSON("_id"
                                      << "rs0"
                                      << "protocolVersion"
                                      << 1
                                      << "version"
                                      << 1
                                      << "configsvr"
                                      << true
                                      << "members"
                                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                               << "localhost:12345")
                                                    << BSON("_id" << 1 << "host"
                                                                  << "localhost:54321"
                                                                  << "priority"
                                                                  << 0
                                                                  << "slaveDelay"
                                                                  << 3)))));
    Status status = configA.validate();
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_STRING_CONTAINS(status.reason(), "cannot have a non-zero slaveDelay");
}

TEST(ReplicaSetConfig, CheckConfigServerMustHaveTrueForWriteConcernMajorityJournalDefault) {
    serverGlobalParams.clusterRole = ClusterRole::ConfigServer;
    ON_BLOCK_EXIT([&] { serverGlobalParams.clusterRole = ClusterRole::None; });
    ReplicaSetConfig configA;
    ASSERT_OK(configA.initialize(BSON("_id"
                                      << "rs0"
                                      << "protocolVersion"
                                      << 1
                                      << "version"
                                      << 1
                                      << "configsvr"
                                      << true
                                      << "members"
                                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                               << "localhost:12345")
                                                    << BSON("_id" << 1 << "host"
                                                                  << "localhost:54321"))
                                      << "writeConcernMajorityJournalDefault"
                                      << false)));
    Status status = configA.validate();
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_STRING_CONTAINS(status.reason(), " must be true in replica set configurations being ");
}

TEST(ReplicaSetConfig, GetPriorityTakeoverDelay) {
    ReplicaSetConfig configA;
    ASSERT_OK(configA.initialize(BSON("_id"
                                      << "rs0"
                                      << "version"
                                      << 1
                                      << "members"
                                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                               << "localhost:12345"
                                                               << "priority"
                                                               << 1)
                                                    << BSON("_id" << 1 << "host"
                                                                  << "localhost:54321"
                                                                  << "priority"
                                                                  << 2)
                                                    << BSON("_id" << 2 << "host"
                                                                  << "localhost:5321"
                                                                  << "priority"
                                                                  << 3)
                                                    << BSON("_id" << 3 << "host"
                                                                  << "localhost:5421"
                                                                  << "priority"
                                                                  << 4)
                                                    << BSON("_id" << 4 << "host"
                                                                  << "localhost:5431"
                                                                  << "priority"
                                                                  << 5))
                                      << "settings"
                                      << BSON("electionTimeoutMillis" << 1000))));
    ASSERT_OK(configA.validate());
    ASSERT_EQUALS(Milliseconds(5000), configA.getPriorityTakeoverDelay(0));
    ASSERT_EQUALS(Milliseconds(4000), configA.getPriorityTakeoverDelay(1));
    ASSERT_EQUALS(Milliseconds(3000), configA.getPriorityTakeoverDelay(2));
    ASSERT_EQUALS(Milliseconds(2000), configA.getPriorityTakeoverDelay(3));
    ASSERT_EQUALS(Milliseconds(1000), configA.getPriorityTakeoverDelay(4));

    ReplicaSetConfig configB;
    ASSERT_OK(configB.initialize(BSON("_id"
                                      << "rs0"
                                      << "version"
                                      << 1
                                      << "members"
                                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                               << "localhost:12345"
                                                               << "priority"
                                                               << 1)
                                                    << BSON("_id" << 1 << "host"
                                                                  << "localhost:54321"
                                                                  << "priority"
                                                                  << 2)
                                                    << BSON("_id" << 2 << "host"
                                                                  << "localhost:5321"
                                                                  << "priority"
                                                                  << 2)
                                                    << BSON("_id" << 3 << "host"
                                                                  << "localhost:5421"
                                                                  << "priority"
                                                                  << 3)
                                                    << BSON("_id" << 4 << "host"
                                                                  << "localhost:5431"
                                                                  << "priority"
                                                                  << 3))
                                      << "settings"
                                      << BSON("electionTimeoutMillis" << 1000))));
    ASSERT_OK(configB.validate());
    ASSERT_EQUALS(Milliseconds(5000), configB.getPriorityTakeoverDelay(0));
    ASSERT_EQUALS(Milliseconds(3000), configB.getPriorityTakeoverDelay(1));
    ASSERT_EQUALS(Milliseconds(3000), configB.getPriorityTakeoverDelay(2));
    ASSERT_EQUALS(Milliseconds(1000), configB.getPriorityTakeoverDelay(3));
    ASSERT_EQUALS(Milliseconds(1000), configB.getPriorityTakeoverDelay(4));
}

TEST(ReplicaSetConfig, ConfirmDefaultValuesOfAndAbilityToSetWriteConcernMajorityJournalDefault) {
    // PV0, should default to false.
    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "version"
                                     << 1
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:12345")))));
    ASSERT_OK(config.validate());
    ASSERT_FALSE(config.getWriteConcernMajorityShouldJournal());
    ASSERT_FALSE(config.toBSON().hasField("writeConcernMajorityJournalDefault"));

    // Should be able to set it true in PV0.
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "version"
                                     << 1
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:12345"))
                                     << "writeConcernMajorityJournalDefault"
                                     << true)));
    ASSERT_OK(config.validate());
    ASSERT_TRUE(config.getWriteConcernMajorityShouldJournal());
    ASSERT_TRUE(config.toBSON().hasField("writeConcernMajorityJournalDefault"));

    // PV1, should default to true.
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "protocolVersion"
                                     << 1
                                     << "version"
                                     << 1
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:12345")))));
    ASSERT_OK(config.validate());
    ASSERT_TRUE(config.getWriteConcernMajorityShouldJournal());
    ASSERT_FALSE(config.toBSON().hasField("writeConcernMajorityJournalDefault"));

    // Should be able to set it false in PV1.
    ASSERT_OK(config.initialize(BSON("_id"
                                     << "rs0"
                                     << "protocolVersion"
                                     << 1
                                     << "version"
                                     << 1
                                     << "members"
                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                              << "localhost:12345"))
                                     << "writeConcernMajorityJournalDefault"
                                     << false)));
    ASSERT_OK(config.validate());
    ASSERT_FALSE(config.getWriteConcernMajorityShouldJournal());
    ASSERT_TRUE(config.toBSON().hasField("writeConcernMajorityJournalDefault"));
}

TEST(ReplicaSetConfig, ReplSetId) {
    // Uninitialized configuration has no ID.
    ASSERT_FALSE(ReplicaSetConfig().hasReplicaSetId());

    // Cannot provide replica set ID in configuration document when initialized from
    // replSetInitiate.
    auto status =
        ReplicaSetConfig().initializeForInitiate(BSON("_id"
                                                      << "rs0"
                                                      << "version"
                                                      << 1
                                                      << "members"
                                                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                               << "localhost:12345"
                                                                               << "priority"
                                                                               << 1))
                                                      << "settings"
                                                      << BSON("replicaSetId" << OID::gen())));
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig, status);
    ASSERT_STRING_CONTAINS(status.reason(),
                           "replica set configuration cannot contain 'replicaSetId' field when "
                           "called from replSetInitiate");


    // Configuration created by replSetInitiate should generate replica set ID.
    ReplicaSetConfig configInitiate;
    ASSERT_OK(
        configInitiate.initializeForInitiate(BSON("_id"
                                                  << "rs0"
                                                  << "version"
                                                  << 1
                                                  << "members"
                                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                           << "localhost:12345"
                                                                           << "priority"
                                                                           << 1)))));
    ASSERT_OK(configInitiate.validate());
    ASSERT_TRUE(configInitiate.hasReplicaSetId());
    OID replicaSetId = configInitiate.getReplicaSetId();

    // Configuration initialized from local database can contain ID.
    ReplicaSetConfig configLocal;
    ASSERT_OK(configLocal.initialize(BSON("_id"
                                          << "rs0"
                                          << "version"
                                          << 1
                                          << "members"
                                          << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                   << "localhost:12345"
                                                                   << "priority"
                                                                   << 1))
                                          << "settings"
                                          << BSON("replicaSetId" << replicaSetId))));
    ASSERT_OK(configLocal.validate());
    ASSERT_TRUE(configLocal.hasReplicaSetId());
    ASSERT_EQUALS(replicaSetId, configLocal.getReplicaSetId());

    // When reconfiguring, we can provide an default ID if the configuration does not contain one.
    OID defaultReplicaSetId = OID::gen();
    ASSERT_OK(configLocal.initialize(BSON("_id"
                                          << "rs0"
                                          << "version"
                                          << 1
                                          << "members"
                                          << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                   << "localhost:12345"
                                                                   << "priority"
                                                                   << 1))),
                                     true,
                                     defaultReplicaSetId));
    ASSERT_OK(configLocal.validate());
    ASSERT_TRUE(configLocal.hasReplicaSetId());
    ASSERT_EQUALS(defaultReplicaSetId, configLocal.getReplicaSetId());

    // 'replicaSetId' field cannot be null.
    status = configLocal.initialize(BSON("_id"
                                         << "rs0"
                                         << "version"
                                         << 1
                                         << "members"
                                         << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                  << "localhost:12345"
                                                                  << "priority"
                                                                  << 1))
                                         << "settings"
                                         << BSON("replicaSetId" << OID())));
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_STRING_CONTAINS(status.reason(), "replicaSetId field value cannot be null");

    // 'replicaSetId' field must be an OID.
    status = configLocal.initialize(BSON("_id"
                                         << "rs0"
                                         << "version"
                                         << 1
                                         << "members"
                                         << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                  << "localhost:12345"
                                                                  << "priority"
                                                                  << 1))
                                         << "settings"
                                         << BSON("replicaSetId" << 12345)));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
    ASSERT_STRING_CONTAINS(status.reason(),
                           "\"replicaSetId\" had the wrong type. Expected objectId, found int");
}

}  // namespace
}  // namespace repl
}  // namespace mongo
