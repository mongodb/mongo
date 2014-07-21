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

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/repl_coordinator_external_state.h"
#include "mongo/db/repl/repl_coordinator_external_state_mock.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replica_set_config_checks.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

    TEST(ValidateConfigForInitiate, VersionMustBe1) {
        ReplicationCoordinatorExternalStateMock rses;
        rses.addSelf(HostAndPort("h1"));

        ReplicaSetConfig config;
        ASSERT_OK(config.initialize(BSON("_id" << "rs0" <<
                                         "version" << 2 <<
                                         "members" << BSON_ARRAY(
                                                 BSON("_id" << 1 << "host" << "h1")))));
        ASSERT_EQUALS(
                ErrorCodes::NewReplicaSetConfigurationIncompatible,
                validateConfigForInitiate(&rses, config).getStatus());
    }

    TEST(ValidateConfigForInitiate, MustFindSelf) {
        ReplicaSetConfig config;
        ASSERT_OK(config.initialize(BSON("_id" << "rs0" <<
                                         "version" << 1 <<
                                         "members" << BSON_ARRAY(
                                                 BSON("_id" << 1 << "host" << "h1") <<
                                                 BSON("_id" << 2 << "host" << "h2") <<
                                                 BSON("_id" << 3 << "host" << "h3")))));
        ReplicationCoordinatorExternalStateMock notPresentExternalState;
        ReplicationCoordinatorExternalStateMock presentOnceExternalState;
        presentOnceExternalState.addSelf(HostAndPort("h2"));
        ReplicationCoordinatorExternalStateMock presentTwiceExternalState;
        presentTwiceExternalState.addSelf(HostAndPort("h3"));
        presentTwiceExternalState.addSelf(HostAndPort("h1"));

        ASSERT_EQUALS(ErrorCodes::NodeNotFound,
                      validateConfigForInitiate(&notPresentExternalState, config).getStatus());
        ASSERT_EQUALS(ErrorCodes::DuplicateKey,
                      validateConfigForInitiate(&presentTwiceExternalState, config).getStatus());
        ASSERT_EQUALS(1, unittest::assertGet(validateConfigForInitiate(&presentOnceExternalState,
                                                                       config)));
    }

    TEST(ValidateConfigForInitiate, SelfMustBeElectable) {
        ReplicaSetConfig config;
        ASSERT_OK(config.initialize(BSON("_id" << "rs0" <<
                                         "version" << 1 <<
                                         "members" << BSON_ARRAY(
                                                 BSON("_id" << 1 << "host" << "h1") <<
                                                 BSON("_id" << 2 << "host" << "h2" <<
                                                      "priority" << 0) <<
                                                 BSON("_id" << 3 << "host" << "h3")))));
        ReplicationCoordinatorExternalStateMock presentOnceExternalState;
        presentOnceExternalState.addSelf(HostAndPort("h2"));

        ASSERT_EQUALS(ErrorCodes::NodeNotElectable,
                      validateConfigForInitiate(&presentOnceExternalState, config).getStatus());
    }

    TEST(ValidateConfigForReconfig, NewConfigVersionNumberMustBeHigherThanOld) {
        ReplicationCoordinatorExternalStateMock externalState;
        externalState.addSelf(HostAndPort("h1"));

        ReplicaSetConfig oldConfig;
        ReplicaSetConfig newConfig;

        // Two configurations, identical except for version.
        ASSERT_OK(oldConfig.initialize(
                          BSON("_id" << "rs0" <<
                               "version" << 1 <<
                               "members" << BSON_ARRAY(
                                                 BSON("_id" << 1 << "host" << "h1") <<
                                                 BSON("_id" << 2 << "host" << "h2") <<
                                                 BSON("_id" << 3 << "host" << "h3")))));

        ASSERT_OK(newConfig.initialize(
                          BSON("_id" << "rs0" <<
                               "version" << 3 <<
                               "members" << BSON_ARRAY(
                                                 BSON("_id" << 1 << "host" << "h1") <<
                                                 BSON("_id" << 2 << "host" << "h2") <<
                                                 BSON("_id" << 3 << "host" << "h3")))));

        ASSERT_OK(oldConfig.validate());
        ASSERT_OK(newConfig.validate());

        // Can reconfig from old to new.
        ASSERT_OK(validateConfigForReconfig(&externalState, oldConfig, newConfig).getStatus());


        // Cannot reconfig from old to old (versions must be different).
        ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                      validateConfigForReconfig(&externalState,
                                                oldConfig,
                                                oldConfig).getStatus());

        // Cannot reconfig from new to old (versions must increase).
        ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                      validateConfigForReconfig(&externalState,
                                                newConfig,
                                                oldConfig).getStatus());
    }

    TEST(ValidateConfigForReconfig, HostAndIdRemappingRestricted) {
        // When reconfiguring a replica set, it is allowed to introduce (host, id) pairs
        // absent from the old config only when the hosts and ids were both individually
        // absent in the old config.

        ReplicationCoordinatorExternalStateMock externalState;
        externalState.addSelf(HostAndPort("h1"));

        ReplicaSetConfig oldConfig;
        ReplicaSetConfig legalNewConfigWithNewHostAndId;
        ReplicaSetConfig illegalNewConfigReusingHost;
        ReplicaSetConfig illegalNewConfigReusingId;

        ASSERT_OK(oldConfig.initialize(
                          BSON("_id" << "rs0" <<
                               "version" << 1 <<
                               "members" << BSON_ARRAY(
                                       BSON("_id" << 1 << "host" << "h1") <<
                                       BSON("_id" << 2 << "host" << "h2") <<
                                       BSON("_id" << 3 << "host" << "h3")))));
        ASSERT_OK(oldConfig.validate());

        //
        // Here, the new config is valid because we've replaced (2, "h2") with
        // (4, "h4"), so neither the member _id or host name were reused.
        //
        ASSERT_OK(legalNewConfigWithNewHostAndId.initialize(
                          BSON("_id" << "rs0" <<
                               "version" << 2 <<
                               "members" << BSON_ARRAY(
                                       BSON("_id" << 1 << "host" << "h1") <<
                                       BSON("_id" << 4 << "host" << "h4") <<
                                       BSON("_id" << 3 << "host" << "h3")))));
        ASSERT_OK(legalNewConfigWithNewHostAndId.validate());
        ASSERT_OK(validateConfigForReconfig(&externalState,
                                            oldConfig,
                                            legalNewConfigWithNewHostAndId).getStatus());

        //
        // Here, the new config is invalid because we've reused host name "h2" with
        // new _id 4.
        //
        ASSERT_OK(illegalNewConfigReusingHost.initialize(
                          BSON("_id" << "rs0" <<
                               "version" << 2 <<
                               "members" << BSON_ARRAY(
                                       BSON("_id" << 1 << "host" << "h1") <<
                                       BSON("_id" << 4 << "host" << "h2") <<
                                       BSON("_id" << 3 << "host" << "h3")))));
        ASSERT_OK(illegalNewConfigReusingHost.validate());
        ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                      validateConfigForReconfig(&externalState,
                                                oldConfig,
                                                illegalNewConfigReusingHost).getStatus());
        //
        // Here, the new config is invalid because we've reused _id 2 with
        // new host name "h4".
        //
        ASSERT_OK(illegalNewConfigReusingId.initialize(
                          BSON("_id" << "rs0" <<
                               "version" << 2 <<
                               "members" << BSON_ARRAY(
                                       BSON("_id" << 1 << "host" << "h1") <<
                                       BSON("_id" << 2 << "host" << "h4") <<
                                       BSON("_id" << 3 << "host" << "h3")))));
        ASSERT_OK(illegalNewConfigReusingId.validate());
        ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                      validateConfigForReconfig(&externalState,
                                                oldConfig,
                                                illegalNewConfigReusingId).getStatus());
    }

    TEST(ValidateConfigForReconfig, MustFindSelf) {
        // Old and new config are same except for version change; this is just testing that we can
        // find ourself in the new config.
        ReplicaSetConfig oldConfig;
        ASSERT_OK(oldConfig.initialize(BSON("_id" << "rs0" <<
                                            "version" << 1 <<
                                            "members" << BSON_ARRAY(
                                                    BSON("_id" << 1 << "host" << "h1") <<
                                                    BSON("_id" << 2 << "host" << "h2") <<
                                                    BSON("_id" << 3 << "host" << "h3")))));

        ReplicaSetConfig newConfig;
        ASSERT_OK(newConfig.initialize(BSON("_id" << "rs0" <<
                                            "version" << 2 <<
                                            "members" << BSON_ARRAY(
                                                    BSON("_id" << 1 << "host" << "h1") <<
                                                    BSON("_id" << 2 << "host" << "h2") <<
                                                    BSON("_id" << 3 << "host" << "h3")))));
        ReplicationCoordinatorExternalStateMock notPresentExternalState;
        ReplicationCoordinatorExternalStateMock presentOnceExternalState;
        presentOnceExternalState.addSelf(HostAndPort("h2"));
        ReplicationCoordinatorExternalStateMock presentTwiceExternalState;
        presentTwiceExternalState.addSelf(HostAndPort("h3"));
        presentTwiceExternalState.addSelf(HostAndPort("h1"));

        ASSERT_EQUALS(ErrorCodes::NodeNotFound,
                      validateConfigForReconfig(&notPresentExternalState,
                                                oldConfig,
                                                newConfig).getStatus());
        ASSERT_EQUALS(ErrorCodes::DuplicateKey,
                      validateConfigForReconfig(&presentTwiceExternalState,
                                                oldConfig,
                                                newConfig).getStatus());
        ASSERT_EQUALS(1, unittest::assertGet(validateConfigForReconfig(&presentOnceExternalState,
                                                                       oldConfig,
                                                                       newConfig)));
    }

    TEST(ValidateConfigForReconfig, SelfMustEndElectable) {
        // Old and new config are same except for version change and the electability of one node;
        // this is just testing that we must be electable in the new config.
        ReplicaSetConfig oldConfig;
        ASSERT_OK(oldConfig.initialize(BSON("_id" << "rs0" <<
                                            "version" << 1 <<
                                            "members" << BSON_ARRAY(
                                                    BSON("_id" << 1 << "host" << "h1") <<
                                                    BSON("_id" << 2 << "host" << "h2") <<
                                                    BSON("_id" << 3 << "host" << "h3")))));

        ReplicaSetConfig newConfig;
        ASSERT_OK(newConfig.initialize(BSON("_id" << "rs0" <<
                                            "version" << 2 <<
                                            "members" << BSON_ARRAY(
                                                    BSON("_id" << 1 << "host" << "h1") <<
                                                    BSON("_id" << 2 << "host" << "h2" <<
                                                         "priority" << 0) <<
                                                    BSON("_id" << 3 << "host" << "h3")))));
        ReplicationCoordinatorExternalStateMock presentOnceExternalState;
        presentOnceExternalState.addSelf(HostAndPort("h2"));

        ASSERT_EQUALS(ErrorCodes::NodeNotElectable,
                      validateConfigForReconfig(&presentOnceExternalState,
                                                oldConfig,
                                                newConfig).getStatus());
    }

}  // namespace
}  // namespace repl
}  // namespace mongo
