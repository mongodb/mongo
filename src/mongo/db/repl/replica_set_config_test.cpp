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

#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

    TEST(ReplicaSetConfig, ParseMinimalConfigAndCheckDefaults) {
        ReplicaSetConfig config;
        ASSERT_OK(config.initialize(
                          BSON("_id" << "rs0" <<
                               "version" << 1 <<
                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                            "host" << "localhost:12345")))));
        ASSERT_OK(config.validate());
        ASSERT_EQUALS("rs0", config.getReplSetName());
        ASSERT_EQUALS(1, config.getConfigVersion());
        ASSERT_EQUALS(1, config.getNumMembers());
        ASSERT_EQUALS(0, config.membersBegin()->getId());
        ASSERT_EQUALS(1, config.getDefaultWriteConcern().wNumNodes);
        ASSERT_EQUALS("", config.getDefaultWriteConcern().wMode);
        ASSERT_EQUALS(10, config.getHeartbeatTimeoutPeriod().total_seconds());
        ASSERT_TRUE(config.isChainingAllowed());
    }

    TEST(ReplicaSetConfig, MajorityCalculationThreeVotersNoArbiters) {
        ReplicaSetConfig config;
        ASSERT_OK(config.initialize(
                          BSON("_id" << "rs0" <<
                               "version" << 2 <<
                               "members" << BSON_ARRAY(
                                       BSON("_id" << 1 << "host" << "h1:1") <<
                                       BSON("_id" << 2 << "host" << "h2:1") <<
                                       BSON("_id" << 3 << "host" << "h3:1") <<
                                       BSON("_id" << 4 << "host" << "h4:1" << "votes" << 0) <<
                                       BSON("_id" << 5 << "host" << "h5:1" << "votes" << 0)))));
        ASSERT_OK(config.validate());
        ASSERT_EQUALS(2, config.getMajorityVoteCount());
    }

    TEST(ReplicaSetConfig, ParseFailsWithBadOrMissingIdField) {
        ReplicaSetConfig config;
        // Replica set name must be a string.
        ASSERT_EQUALS(
                ErrorCodes::TypeMismatch,
                config.initialize(
                        BSON("_id" << 1 <<
                             "version" << 1 <<
                             "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                          "host" << "localhost:12345")))));

        // Replica set name must be present.
        ASSERT_EQUALS(
                ErrorCodes::NoSuchKey,
                config.initialize(
                        BSON("version" << 1 <<
                             "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                          "host" << "localhost:12345")))));

        // Empty repl set name parses, but does not validate.
        ASSERT_OK(config.initialize(
                          BSON("_id" << "" <<
                               "version" << 1 <<
                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                            "host" << "localhost:12345")))));

        ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());
    }

    TEST(ReplicaSetConfig, ParseFailsWithBadOrMissingVersionField) {
        ReplicaSetConfig config;
        // Config version field must be present.
        ASSERT_EQUALS(
                ErrorCodes::NoSuchKey,
                config.initialize(
                        BSON("_id" << "rs0" <<
                             "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                          "host" << "localhost:12345")))));
        ASSERT_EQUALS(
                ErrorCodes::TypeMismatch,
                config.initialize(
                        BSON("_id" << "rs0" <<
                             "version" << "1" <<
                             "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                          "host" << "localhost:12345")))));

        ASSERT_OK(config.initialize(
                          BSON("_id" << "rs0" <<
                               "version" << 1.0 <<
                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                            "host" << "localhost:12345")))));
        ASSERT_OK(config.validate());
        ASSERT_OK(config.initialize(
                          BSON("_id" << "rs0" <<
                               "version" << 0.0 <<
                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                            "host" << "localhost:12345")))));
        ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());
        ASSERT_OK(config.initialize(
                          BSON("_id" << "rs0" <<
                               "version" <<
                               static_cast<long long>(std::numeric_limits<int>::max()) + 1 <<
                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                            "host" << "localhost:12345")))));
        ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());
    }

    TEST(ReplicaSetConfig, ParseFailsWithBadMembers) {
        ReplicaSetConfig config;
        ASSERT_EQUALS(ErrorCodes::TypeMismatch,
                      config.initialize(
                          BSON("_id" << "rs0" <<
                               "version" << 1 <<
                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                            "host" << "localhost:12345") <<
                                                       "localhost:23456"))));
        ASSERT_EQUALS(ErrorCodes::NoSuchKey,
                      config.initialize(
                          BSON("_id" << "rs0" <<
                               "version" << 1 <<
                               "members" << BSON_ARRAY(BSON("host" << "localhost:12345")))));
    }

    TEST(ReplicaSetConfig, ParseFailsWithLocalNonLocalHostMix) {
        ReplicaSetConfig config;
        ASSERT_OK(config.initialize(BSON("_id" << "rs0" <<
                                         "version" << 1 <<
                                         "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                                      "host" << "localhost") <<
                                                                 BSON("_id" << 1 <<
                                                                      "host" << "otherhost")))));
        ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());
    }

    TEST(ReplicaSetConfig, ParseFailsWithNoElectableNodes) {
        ReplicaSetConfig config;
        const BSONObj configBsonNoElectableNodes = BSON(
                "_id" << "rs0" <<
                "version" << 1 <<
                "members" << BSON_ARRAY(
                        BSON("_id" << 0 << "host" << "localhost:1" << "priority" << 0) <<
                        BSON("_id" << 1 << "host" << "localhost:2" << "priority" << 0)));

        ASSERT_OK(config.initialize(configBsonNoElectableNodes));
        ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());

        const BSONObj configBsonNoElectableNodesOneArbiter = BSON(
                "_id" << "rs0" <<
                "version" << 1 <<
                "members" << BSON_ARRAY(
                        BSON("_id" << 0 << "host" << "localhost:1" << "arbiterOnly" << 1) <<
                        BSON("_id" << 1 << "host" << "localhost:2" << "priority" << 0)));

        ASSERT_OK(config.initialize(configBsonNoElectableNodesOneArbiter));
        ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());

        const BSONObj configBsonNoElectableNodesTwoArbiters = BSON(
                "_id" << "rs0" <<
                "version" << 1 <<
                "members" << BSON_ARRAY(
                        BSON("_id" << 0 << "host" << "localhost:1" << "arbiterOnly" << 1) <<
                        BSON("_id" << 1 << "host" << "localhost:2" << "arbiterOnly" << 1)));

        ASSERT_OK(config.initialize(configBsonNoElectableNodesOneArbiter));
        ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());

        const BSONObj configBsonOneElectableNode = BSON(
                "_id" << "rs0" <<
                "version" << 1 <<
                "members" << BSON_ARRAY(
                        BSON("_id" << 0 << "host" << "localhost:1" << "priority" << 0) <<
                        BSON("_id" << 1 << "host" << "localhost:2" << "priority" << 1)));
        ASSERT_OK(config.initialize(configBsonOneElectableNode));
        ASSERT_OK(config.validate());
    }

    TEST(ReplicaSetConfig, ParseFailsWithTooFewVoters) {
        ReplicaSetConfig config;
        const BSONObj configBsonNoVoters = BSON(
                "_id" << "rs0" <<
                "version" << 1 <<
                "members" << BSON_ARRAY(
                        BSON("_id" << 0 << "host" << "localhost:1" << "votes" << 0) <<
                        BSON("_id" << 1 << "host" << "localhost:2" << "votes" << 0)));

        ASSERT_OK(config.initialize(configBsonNoVoters));
        ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());

        const BSONObj configBsonOneVoter = BSON(
                "_id" << "rs0" <<
                "version" << 1 <<
                "members" << BSON_ARRAY(
                        BSON("_id" << 0 << "host" << "localhost:1" << "votes" << 0) <<
                        BSON("_id" << 1 << "host" << "localhost:2" << "votes" << 1)));
        ASSERT_OK(config.initialize(configBsonOneVoter));
        ASSERT_OK(config.validate());
    }

    TEST(ReplicaSetConfig, ParseFailsWithTooManyVoters) {
        ReplicaSetConfig config;
        namespace mmb = mutablebson;
        mmb::Document configDoc;
        mmb::Element configDocRoot = configDoc.root();
        ASSERT_OK(configDocRoot.appendString("_id", "rs0"));
        ASSERT_OK(configDocRoot.appendInt("version", 1));
        mmb::Element membersArray = configDoc.makeElementArray("members");
        ASSERT_OK(configDocRoot.pushBack(membersArray));
        for (size_t i = 0; i < ReplicaSetConfig::kMaxVotingMembers + 1; ++i) {
            mmb::Element memberElement = configDoc.makeElementObject("");
            ASSERT_OK(membersArray.pushBack(memberElement));
            ASSERT_OK(memberElement.appendInt("_id", i));
            ASSERT_OK(memberElement.appendString(
                              "host", std::string(str::stream() << "localhost" << i + 1)));
            ASSERT_OK(memberElement.appendInt("votes", 1));
        }

        const BSONObj configBsonTooManyVoters = configDoc.getObject();

        membersArray.leftChild().findFirstChildNamed("votes").setValueInt(0);
        const BSONObj configBsonMaxVoters = configDoc.getObject();


        ASSERT_OK(config.initialize(configBsonMaxVoters));
        ASSERT_OK(config.validate());
        ASSERT_OK(config.initialize(configBsonTooManyVoters));
        ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());
    }

    TEST(ReplicaSetConfig, ParseFailsWithDuplicateHost) {
        ReplicaSetConfig config;
        const BSONObj configBson = BSON(
                "_id" << "rs0" <<
                "version" << 1 <<
                "members" << BSON_ARRAY(
                        BSON("_id" << 0 << "host" << "localhost:1") <<
                        BSON("_id" << 1 << "host" << "localhost:1")));
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
            ASSERT_OK(memberElement.appendString(
                              "host", std::string(str::stream() << "localhost" << i + 1)));
            if (i >= ReplicaSetConfig::kMaxVotingMembers) {
                ASSERT_OK(memberElement.appendInt("votes", 0));
            }
        }
        const BSONObj configBsonMaxNodes = configDoc.getObject();

        mmb::Element memberElement = configDoc.makeElementObject("");
        ASSERT_OK(membersArray.pushBack(memberElement));
        ASSERT_OK(memberElement.appendInt("_id", ReplicaSetConfig::kMaxMembers));
        ASSERT_OK(memberElement.appendString(
                          "host", std::string(str::stream() <<
                                              "localhost" << ReplicaSetConfig::kMaxMembers + 1)));
        ASSERT_OK(memberElement.appendInt("votes", 0));
        const BSONObj configBsonTooManyNodes = configDoc.getObject();


        ASSERT_OK(config.initialize(configBsonMaxNodes));
        ASSERT_OK(config.validate());
        ASSERT_OK(config.initialize(configBsonTooManyNodes));
        ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());
    }

    TEST(ReplicaSetConfig, ParseFailsWithUnexpectedField) {
        ReplicaSetConfig config;
        Status status = config.initialize(BSON("_id" << "rs0" <<
                                               "version" << 1 <<
                                               "unexpectedfield" << "value"));
        ASSERT_EQUALS(ErrorCodes::BadValue, status);
    }

    TEST(ReplicaSetConfig, ParseFailsWithNonArrayMembersField) {
        ReplicaSetConfig config;
        Status status = config.initialize(BSON("_id" << "rs0" <<
                                               "version" << 1 <<
                                               "members" << "value"));
        ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
    }

    TEST(ReplicaSetConfig, ParseFailsWithNonNumericHeartbeatTimeoutSecsField) {
        ReplicaSetConfig config;
        Status status = config.initialize(BSON("_id" << "rs0" <<
                                               "version" << 1 <<
                                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                       "host" << "localhost:12345")) <<
                                               "settings" << BSON("heartbeatTimeoutSecs" << "no")));
        ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
    }

    TEST(ReplicaSetConfig, ParseFailsWithNonBoolChainingAllowedField) {
        ReplicaSetConfig config;
        Status status = config.initialize(BSON("_id" << "rs0" <<
                                               "version" << 1 <<
                                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                       "host" << "localhost:12345")) <<
                                               "settings" << BSON("chainingAllowed" << "no")));
        ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
    }

    TEST(ReplicaSetConfig, ParseFailsWithNonObjectSettingsField) {
        ReplicaSetConfig config;
        Status status = config.initialize(BSON("_id" << "rs0" <<
                                               "version" << 1 <<
                                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                       "host" << "localhost:12345")) <<
                                               "settings" << "none"));
        ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
    }

    TEST(ReplicaSetConfig, ParseFailsWithGetLastErrorDefaultsFieldUnparseable) {
        ReplicaSetConfig config;
        Status status = config.initialize(BSON("_id" << "rs0" <<
                                               "version" << 1 <<
                                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                       "host" << "localhost:12345")) <<
                                               "settings" << BSON("getLastErrorDefaults" << BSON(
                                                       "fsync" << "seven"))));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status);
    }

    TEST(ReplicaSetConfig, ParseFailsWithNonObjectGetLastErrorDefaultsField) {
        ReplicaSetConfig config;
        Status status = config.initialize(BSON("_id" << "rs0" <<
                                               "version" << 1 <<
                                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                       "host" << "localhost:12345")) <<
                                               "settings" << BSON("getLastErrorDefaults" << "no")));
        ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
    }

    TEST(ReplicaSetConfig, ParseFailsWithNonObjectGetLastErrorModesField) {
        ReplicaSetConfig config;
        Status status = config.initialize(BSON("_id" << "rs0" <<
                                               "version" << 1 <<
                                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                       "host" << "localhost:12345")) <<
                                               "settings" << BSON("getLastErrorModes" << "no")));
        ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
    }

    TEST(ReplicaSetConfig, ParseFailsWithDuplicateGetLastErrorModesField) {
        ReplicaSetConfig config;
        Status status = config.initialize(BSON("_id" << "rs0" <<
                                               "version" << 1 <<
                                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                       "host" << "localhost:12345" << 
                                                       "tags" << BSON("tag" << "yes"))) <<
                                               "settings" << BSON("getLastErrorModes" << BSON(
                                                        "one" << BSON("tag" << 1) <<
                                                        "one" << BSON("tag" << 1)))));
        ASSERT_EQUALS(ErrorCodes::DuplicateKey, status);
    }

    TEST(ReplicaSetConfig, ParseFailsWithNonObjectGetLastErrorModesEntryField) {
        ReplicaSetConfig config;
        Status status = config.initialize(BSON("_id" << "rs0" <<
                                               "version" << 1 <<
                                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                       "host" << "localhost:12345" << 
                                                       "tags" << BSON("tag" << "yes"))) <<
                                               "settings" << BSON("getLastErrorModes" << BSON(
                                                        "one" << 1))));
        ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
    }

    TEST(ReplicaSetConfig, ParseFailsWithNonNumericGetLastErrorModesConstraintValue) {
        ReplicaSetConfig config;
        Status status = config.initialize(BSON("_id" << "rs0" <<
                                               "version" << 1 <<
                                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                       "host" << "localhost:12345" << 
                                                       "tags" << BSON("tag" << "yes"))) <<
                                               "settings" << BSON("getLastErrorModes" << BSON(
                                                        "one" << BSON("tag" << "no")))));
        ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
    }

    TEST(ReplicaSetConfig, ParseFailsWithNegativeGetLastErrorModesConstraintValue) {
        ReplicaSetConfig config;
        Status status = config.initialize(BSON("_id" << "rs0" <<
                                               "version" << 1 <<
                                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                       "host" << "localhost:12345" << 
                                                       "tags" << BSON("tag" << "yes"))) <<
                                               "settings" << BSON("getLastErrorModes" << BSON(
                                                        "one" << BSON("tag" << -1)))));
        ASSERT_EQUALS(ErrorCodes::BadValue, status);
    }

    TEST(ReplicaSetConfig, ParseFailsWithNonExistentGetLastErrorModesConstraintTag) {
        ReplicaSetConfig config;
        Status status = config.initialize(BSON("_id" << "rs0" <<
                                               "version" << 1 <<
                                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                       "host" << "localhost:12345" << 
                                                       "tags" << BSON("tag" << "yes"))) <<
                                               "settings" << BSON("getLastErrorModes" << BSON(
                                                        "one" << BSON("tag2" << 1)))));
        ASSERT_EQUALS(ErrorCodes::NoSuchKey, status);
    }

    TEST(ReplicaSetConfig, ValidateFailsWithDuplicateMemberId) {
        ReplicaSetConfig config;
        Status status = config.initialize(BSON("_id" << "rs0" <<
                                               "version" << 1 <<
                                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                       "host" << "localhost:12345") <<
                                                       BSON("_id" << 0 <<
                                                       "host" << "someoneelse:12345"))));
        ASSERT_OK(status);

        status = config.validate();
        ASSERT_EQUALS(ErrorCodes::BadValue, status);
    }

    TEST(ReplicaSetConfig, ValidateFailsWithInvalidMember) {
        ReplicaSetConfig config;
        Status status = config.initialize(BSON("_id" << "rs0" <<
                                               "version" << 1 <<
                                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                       "host" << "localhost:12345" <<
                                                       "hidden" << true))));
        ASSERT_OK(status);

        status = config.validate();
        ASSERT_EQUALS(ErrorCodes::BadValue, status);
    }

    TEST(ReplicaSetConfig, ChainingAllowedField) {
        ReplicaSetConfig config;
        ASSERT_OK(config.initialize(
                          BSON("_id" << "rs0" <<
                               "version" << 1 <<
                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                            "host" << "localhost:12345")) <<
                               "settings" << BSON("chainingAllowed" << true))));
        ASSERT_OK(config.validate());
        ASSERT_TRUE(config.isChainingAllowed());

        ASSERT_OK(config.initialize(
                          BSON("_id" << "rs0" <<
                               "version" << 1 <<
                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                            "host" << "localhost:12345")) <<
                               "settings" << BSON("chainingAllowed" << false))));
        ASSERT_OK(config.validate());
        ASSERT_FALSE(config.isChainingAllowed());
    }

    TEST(ReplicaSetConfig, HeartbeatTimeoutField) {
        ReplicaSetConfig config;
        ASSERT_OK(config.initialize(
                          BSON("_id" << "rs0" <<
                               "version" << 1 <<
                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                            "host" << "localhost:12345")) <<
                               "settings" << BSON("heartbeatTimeoutSecs" << 20))));
        ASSERT_OK(config.validate());
        ASSERT_EQUALS(20, config.getHeartbeatTimeoutPeriod().total_seconds());

        ASSERT_OK(config.initialize(
                          BSON("_id" << "rs0" <<
                               "version" << 1 <<
                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                            "host" << "localhost:12345")) <<
                               "settings" << BSON("heartbeatTimeoutSecs" << -20))));
        ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());
    }

    TEST(ReplicaSetConfig, GleDefaultField) {
        ReplicaSetConfig config;
        ASSERT_OK(config.initialize(
                          BSON("_id" << "rs0" <<
                               "version" << 1 <<
                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                            "host" << "localhost:12345")) <<
                               "settings" << BSON(
                                       "getLastErrorDefaults" << BSON("w" << "majority")))));
        ASSERT_OK(config.validate());
        ASSERT_EQUALS("majority", config.getDefaultWriteConcern().wMode);

        ASSERT_OK(config.initialize(
                          BSON("_id" << "rs0" <<
                               "version" << 1 <<
                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                            "host" << "localhost:12345")) <<
                               "settings" << BSON(
                                       "getLastErrorDefaults" << BSON("w" << "frim")))));
        ASSERT_EQUALS(ErrorCodes::BadValue, config.validate());

        ASSERT_OK(config.initialize(
                          BSON("_id" << "rs0" <<
                               "version" << 1 <<
                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                            "host" << "localhost:12345" <<
                                                            "tags" << BSON("a" << "v"))) <<
                               "settings" << BSON(
                                       "getLastErrorDefaults" << BSON("w" << "frim") <<
                                       "getLastErrorModes" << BSON("frim" << BSON("a" << 1))))));
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
        return a.getId() == b.getId() &&
                a.getHostAndPort() == b.getHostAndPort() &&
                a.getPriority() == b.getPriority() &&
                a.getSlaveDelay() == b.getSlaveDelay() &&
                a.isVoter() == b.isVoter() &&
                a.isArbiter() == b.isArbiter() &&
                a.isHidden() == b.isHidden() &&
                a.shouldBuildIndexes() == b.shouldBuildIndexes() &&
                a.getNumTags() == b.getNumTags();
    }

    bool operator==(const ReplicaSetConfig& a, const ReplicaSetConfig& b) {
        // compare WriteConcernModes
        std::vector<std::string> modeNames = a.getWriteConcernNames();
        for (std::vector<std::string>::iterator it = modeNames.begin();
                it != modeNames.end();
                it++) {
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
        for (ReplicaSetConfig::MemberIterator memA = a.membersBegin();
                memA != a.membersEnd();
                memA++) {
            bool same = false;
            for (ReplicaSetConfig::MemberIterator memB = b.membersBegin();
                    memB != b.membersEnd();
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
                a.getConfigVersion() == b.getConfigVersion() &&
                a.getNumMembers() == b.getNumMembers() &&
                a.getHeartbeatTimeoutPeriod() == b.getHeartbeatTimeoutPeriod() &&
                a.isChainingAllowed() == b.isChainingAllowed() &&
                a.getDefaultWriteConcern().wNumNodes == b.getDefaultWriteConcern().wNumNodes &&
                a.getDefaultWriteConcern().wMode == b.getDefaultWriteConcern().wMode;
    }

    TEST(ReplicaSetConfig, toBSONRoundTripAbility) {
        ReplicaSetConfig configA;
        ReplicaSetConfig configB;
        ASSERT_OK(configA.initialize(
                          BSON("_id" << "rs0" <<
                               "version" << 1 <<
                               "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                            "host" << "localhost:12345")) <<
                               "settings" << BSON("heartbeatTimeoutSecs" << 20))));
        ASSERT_OK(configB.initialize(configA.toBSON()));
        ASSERT_TRUE(configA == configB);
    }

    TEST(ReplicaSetConfig, toBSONRoundTripAbilityLarge) {
        ReplicaSetConfig configA;
        ReplicaSetConfig configB;
        ASSERT_OK(configA.initialize(
                BSON("_id" << "asdf"
                  << "version" << 9
                  << "members" << BSON_ARRAY(
                          BSON("_id" << 0
                            << "host" << "localhost:12345"
                            << "arbiterOnly" << true
                            << "votes" << 1
                          ) <<
                          BSON("_id" << 3
                            << "host" << "localhost:3828"
                            << "arbiterOnly" << false
                            << "hidden" << true
                            << "buildIndexes" << false
                            << "priority" << 0
                            << "slaveDelay" << 17
                            << "votes" << 0
                            << "tags" << BSON("coast" << "east" << "ssd" << "true")
                          ) <<
                          BSON("_id" << 2
                            << "host" << "foo.com:3828"
                            << "priority" << 9
                            << "votes" << 0
                            << "tags" << BSON("coast" << "west" << "hdd" << "true")
                          ))
                  << "settings" << BSON("heartbeatTimeoutSecs" << 20
                                     << "chainingAllowd" << true
                                     << "getLastErrorDefaults" << BSON("w" << "majority")
                                     << "getLastErrorModes" << BSON(
                                            "disks" << BSON("ssd" << 1 << "hdd" << 1)
                                            << "coasts" << BSON("coast" << 2)))
                                     )));
        ASSERT_OK(configB.initialize(configA.toBSON()));
        ASSERT_TRUE(configA == configB);
    }

    TEST(ReplicaSetConfig, toBSONRoundTripAbilityInvalid) {
        ReplicaSetConfig configA;
        ReplicaSetConfig configB;
        ASSERT_OK(configA.initialize(
                BSON("_id" << ""
                  << "version" << -3
                  << "members" << BSON_ARRAY(
                          BSON("_id" << 0
                            << "host" << "localhost:12345"
                            << "arbiterOnly" << true
                            << "votes" << 0
                          ) <<
                          BSON("_id" << 0
                            << "host" << "localhost:3828"
                            << "arbiterOnly" << false
                            << "buildIndexes" << false
                            << "priority" << 2
                          ) <<
                          BSON("_id" << 2
                            << "host" << "localhost:3828"
                            << "priority" << 9
                            << "votes" << 0
                          ))
                  << "settings" << BSON("heartbeatTimeoutSecs" << -20))));
        ASSERT_OK(configB.initialize(configA.toBSON()));
        ASSERT_NOT_OK(configA.validate());
        ASSERT_NOT_OK(configB.validate());
        ASSERT_TRUE(configA == configB);
    }

    TEST(ReplicaSetConfig, CheckIfWriteConcernCanBeSatisfied) {
        ReplicaSetConfig configA;
        ASSERT_OK(configA.initialize(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                 "host" << "node0" <<
                                                 "tags" << BSON("dc" << "NA" <<
                                                                "rack" << "rackNA1")) <<
                                             BSON("_id" << 1 <<
                                                  "host" << "node1" <<
                                                  "tags" << BSON("dc" << "NA" <<
                                                                 "rack" << "rackNA2")) <<
                                             BSON("_id" << 2 <<
                                                  "host" << "node2" <<
                                                  "tags" << BSON("dc" << "NA" <<
                                                                 "rack" << "rackNA3")) <<
                                             BSON("_id" << 3 <<
                                                  "host" << "node3" <<
                                                  "tags" << BSON("dc" << "EU" <<
                                                                 "rack" << "rackEU1")) <<
                                             BSON("_id" << 4 <<
                                                  "host" << "node4" <<
                                                  "tags" << BSON("dc" << "EU" <<
                                                                 "rack" << "rackEU2")) <<
                                             BSON("_id" << 5 <<
                                                  "host" << "node5" <<
                                                  "arbiterOnly" << true)) <<
                     "settings" << BSON("getLastErrorModes" <<
                             BSON("valid" << BSON("dc" << 2 << "rack" << 3) <<
                                  "invalidNotEnoughValues" << BSON("dc" << 3) <<
                                  "invalidNotEnoughNodes" << BSON("rack" << 6))))));

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

}  // namespace
}  // namespace repl
}  // namespace mongo
