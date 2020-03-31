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

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_set_config_checks.h"
#include "mongo/db/repl/replication_coordinator_external_state.h"
#include "mongo/db/repl/replication_coordinator_external_state_mock.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/ensure_fcv.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

using unittest::EnsureFCV;

TEST_F(ServiceContextTest, ValidateConfigForInitiate_VersionMustBe1) {
    ReplicationCoordinatorExternalStateMock rses;
    rses.addSelf(HostAndPort("h1"));

    ReplSetConfig config;
    ASSERT_OK(
        config.initializeForInitiate(BSON("_id"
                                          << "rs0"
                                          << "version" << 2 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")))));
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForInitiate(&rses, config, getGlobalServiceContext()).getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForInitiate_TermIsAlwaysUninitializedTerm) {
    ReplicationCoordinatorExternalStateMock rses;
    rses.addSelf(HostAndPort("h1"));

    ReplSetConfig config;
    ASSERT_OK(config.initializeForInitiate(BSON("_id"
                                                << "rs0"
                                                << "version" << 1 << "term" << 999
                                                << "protocolVersion" << 1 << "members"
                                                << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                         << "h1")))));
    ASSERT_OK(validateConfigForInitiate(&rses, config, getGlobalServiceContext()).getStatus());
    ASSERT_EQUALS(config.getConfigTerm(), OpTime::kUninitializedTerm);
}

TEST_F(ServiceContextTest, ValidateConfigForInitiate_MustFindSelf) {
    ReplSetConfig config;
    ASSERT_OK(
        config.initializeForInitiate(BSON("_id"
                                          << "rs0"
                                          << "version" << 1 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "h2")
                                                        << BSON("_id" << 3 << "host"
                                                                      << "h3")))));
    ReplicationCoordinatorExternalStateMock notPresentExternalState;
    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ReplicationCoordinatorExternalStateMock presentTwiceExternalState;
    presentTwiceExternalState.addSelf(HostAndPort("h3"));
    presentTwiceExternalState.addSelf(HostAndPort("h1"));

    ASSERT_EQUALS(
        ErrorCodes::NodeNotFound,
        validateConfigForInitiate(&notPresentExternalState, config, getGlobalServiceContext())
            .getStatus());
    ASSERT_EQUALS(
        ErrorCodes::InvalidReplicaSetConfig,
        validateConfigForInitiate(&presentTwiceExternalState, config, getGlobalServiceContext())
            .getStatus());
    ASSERT_EQUALS(1,
                  unittest::assertGet(validateConfigForInitiate(
                      &presentOnceExternalState, config, getGlobalServiceContext())));
}

TEST_F(ServiceContextTest, ValidateConfigForInitiate_SelfMustBeElectable) {
    ReplSetConfig config;
    ASSERT_OK(
        config.initializeForInitiate(BSON("_id"
                                          << "rs0"
                                          << "version" << 1 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "h2"
                                                                      << "priority" << 0)
                                                        << BSON("_id" << 3 << "host"
                                                                      << "h3")))));
    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));

    ASSERT_EQUALS(
        ErrorCodes::NodeNotElectable,
        validateConfigForInitiate(&presentOnceExternalState, config, getGlobalServiceContext())
            .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForInitiate_WriteConcernMustBeSatisfiable) {
    ReplSetConfig config;
    ASSERT_OK(
        config.initializeForInitiate(BSON("_id"
                                          << "rs0"
                                          << "version" << 1 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1"))
                                          << "settings"
                                          << BSON("getLastErrorDefaults" << BSON("w" << 2)))));
    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));

    ASSERT_EQUALS(
        ErrorCodes::UnsatisfiableWriteConcern,
        validateConfigForInitiate(&presentOnceExternalState, config, getGlobalServiceContext())
            .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForInitiate_ArbiterPriorityMustBeZeroOrOne) {
    ReplSetConfig zeroConfig;
    ReplSetConfig oneConfig;
    ReplSetConfig twoConfig;
    ASSERT_OK(zeroConfig.initializeForInitiate(
        BSON("_id"
             << "rs0"
             << "version" << 1 << "protocolVersion" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                      << "h1")
                           << BSON("_id" << 2 << "host"
                                         << "h2"
                                         << "priority" << 0 << "arbiterOnly" << true)
                           << BSON("_id" << 3 << "host"
                                         << "h3")))));

    ASSERT_OK(oneConfig.initializeForInitiate(
        BSON("_id"
             << "rs0"
             << "version" << 1 << "protocolVersion" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                      << "h1")
                           << BSON("_id" << 2 << "host"
                                         << "h2"
                                         << "priority" << 1 << "arbiterOnly" << true)
                           << BSON("_id" << 3 << "host"
                                         << "h3")))));

    ASSERT_OK(twoConfig.initializeForInitiate(
        BSON("_id"
             << "rs0"
             << "version" << 1 << "protocolVersion" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                      << "h1")
                           << BSON("_id" << 2 << "host"
                                         << "h2"
                                         << "priority" << 2 << "arbiterOnly" << true)
                           << BSON("_id" << 3 << "host"
                                         << "h3")))));
    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h1"));

    ASSERT_OK(
        validateConfigForInitiate(&presentOnceExternalState, zeroConfig, getGlobalServiceContext())
            .getStatus());
    ASSERT_OK(
        validateConfigForInitiate(&presentOnceExternalState, oneConfig, getGlobalServiceContext())
            .getStatus());
    ASSERT_EQUALS(
        ErrorCodes::InvalidReplicaSetConfig,
        validateConfigForInitiate(&presentOnceExternalState, twoConfig, getGlobalServiceContext())
            .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForInitiate_NewlyAddedFieldNotAllowed) {
    // Set the flag to add the 'newlyAdded' field to MemberConfigs.
    enableAutomaticReconfig = true;
    // Set the flag back to false after this test exits.
    ON_BLOCK_EXIT([] { enableAutomaticReconfig = false; });
    ReplSetConfig firstNewlyAdded;
    ReplSetConfig lastNewlyAdded;
    ASSERT_OK(
        firstNewlyAdded.initializeForInitiate(BSON("_id"
                                                   << "rs0"
                                                   << "version" << 1 << "protocolVersion" << 1
                                                   << "members"
                                                   << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                            << "newly_added_h1"
                                                                            << "newlyAdded" << true)
                                                                 << BSON("_id" << 2 << "host"
                                                                               << "h2")
                                                                 << BSON("_id" << 3 << "host"
                                                                               << "h3")))));

    ASSERT_OK(lastNewlyAdded.initializeForInitiate(
        BSON("_id"
             << "rs0"
             << "version" << 1 << "protocolVersion" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                      << "h1")
                           << BSON("_id" << 2 << "host"
                                         << "h2")
                           << BSON("_id" << 3 << "host"
                                         << "newly_added_h3"
                                         << "newlyAdded" << true)))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h1"));

    auto status = validateConfigForInitiate(
                      &presentOnceExternalState, firstNewlyAdded, getGlobalServiceContext())
                      .getStatus();
    ASSERT_EQUALS(status, ErrorCodes::InvalidReplicaSetConfig);
    ASSERT_TRUE(status.reason().find("newly_added_h1") != std::string::npos);
    status = validateConfigForInitiate(
                 &presentOnceExternalState, lastNewlyAdded, getGlobalServiceContext())
                 .getStatus();
    ASSERT_EQUALS(status, ErrorCodes::InvalidReplicaSetConfig);
    ASSERT_TRUE(status.reason().find("newly_added_h3") != std::string::npos);
}

TEST_F(ServiceContextTest, ValidateConfigForReconfig_NewConfigVersionNumberMustBeHigherThanOld) {
    ReplicationCoordinatorExternalStateMock externalState;
    externalState.addSelf(HostAndPort("h1"));

    ReplSetConfig oldConfig;
    ReplSetConfig newConfig;

    // Two configurations, identical except for version.
    ASSERT_OK(oldConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                 << "h1")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "h2")
                                                      << BSON("_id" << 3 << "host"
                                                                    << "h3")))));

    ASSERT_OK(newConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 3 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                 << "h1")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "h2")
                                                      << BSON("_id" << 3 << "host"
                                                                    << "h3")))));

    ASSERT_OK(oldConfig.validate());
    ASSERT_OK(newConfig.validate());

    // Can reconfig from old to new.
    ASSERT_OK(validateConfigForReconfig(
                  &externalState, oldConfig, newConfig, getGlobalServiceContext(), false)
                  .getStatus());


    // Cannot reconfig from old to old (versions must be different).
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(
                      &externalState, oldConfig, oldConfig, getGlobalServiceContext(), false)
                      .getStatus());

    // Cannot reconfig from new to old (versions must increase).
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(
                      &externalState, newConfig, oldConfig, getGlobalServiceContext(), false)
                      .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForReconfig_NewConfigMustNotChangeSetName) {
    ReplicationCoordinatorExternalStateMock externalState;
    externalState.addSelf(HostAndPort("h1"));

    ReplSetConfig oldConfig;
    ReplSetConfig newConfig;

    // Two configurations, compatible except for set name.
    ASSERT_OK(oldConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                 << "h1")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "h2")
                                                      << BSON("_id" << 3 << "host"
                                                                    << "h3")))));

    ASSERT_OK(newConfig.initialize(BSON("_id"
                                        << "rs1"
                                        << "version" << 3 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                 << "h1")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "h2")
                                                      << BSON("_id" << 3 << "host"
                                                                    << "h3")))));

    ASSERT_OK(oldConfig.validate());
    ASSERT_OK(newConfig.validate());
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(
                      &externalState, oldConfig, newConfig, getGlobalServiceContext(), false)
                      .getStatus());
    // Forced reconfigs also do not allow this.
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(
                      &externalState, newConfig, oldConfig, getGlobalServiceContext(), true)
                      .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForReconfig_NewConfigMustNotChangeSetId) {
    ReplicationCoordinatorExternalStateMock externalState;
    externalState.addSelf(HostAndPort("h1"));

    ReplSetConfig oldConfig;
    ReplSetConfig newConfig;

    // Two configurations, compatible except for set ID.
    ASSERT_OK(oldConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                 << "h1")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "h2")
                                                      << BSON("_id" << 3 << "host"
                                                                    << "h3"))
                                        << "settings" << BSON("replicaSetId" << OID::gen()))));

    ASSERT_OK(newConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 3 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                 << "h1")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "h2")
                                                      << BSON("_id" << 3 << "host"
                                                                    << "h3"))
                                        << "settings" << BSON("replicaSetId" << OID::gen()))));

    ASSERT_OK(oldConfig.validate());
    ASSERT_OK(newConfig.validate());
    const auto status = validateConfigForReconfig(
                            &externalState, oldConfig, newConfig, getGlobalServiceContext(), false)
                            .getStatus();
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible, status);
    ASSERT_STRING_CONTAINS(status.reason(), "New and old configurations differ in replica set ID");

    // Forced reconfigs also do not allow this.
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(
                      &externalState, newConfig, oldConfig, getGlobalServiceContext(), true)
                      .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForReconfig_NewConfigMustNotFlipBuildIndexesFlag) {
    ReplicationCoordinatorExternalStateMock externalState;
    externalState.addSelf(HostAndPort("h1"));

    ReplSetConfig oldConfig;
    ReplSetConfig newConfig;
    ReplSetConfig oldConfigRefresh;

    // Three configurations, two compatible except that h2 flips the buildIndex flag.
    // The third, compatible with the first.
    ASSERT_OK(oldConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                 << "h1")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "h2"
                                                                    << "buildIndexes" << false
                                                                    << "priority" << 0)
                                                      << BSON("_id" << 3 << "host"
                                                                    << "h3")))));

    ASSERT_OK(newConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 3 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                 << "h1")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "h2"
                                                                    << "buildIndexes" << true
                                                                    << "priority" << 0)
                                                      << BSON("_id" << 3 << "host"
                                                                    << "h3")))));

    ASSERT_OK(
        oldConfigRefresh.initialize(BSON("_id"
                                         << "rs0"
                                         << "version" << 2 << "protocolVersion" << 1 << "members"
                                         << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                  << "h1")
                                                       << BSON("_id" << 2 << "host"
                                                                     << "h2"
                                                                     << "buildIndexes" << false
                                                                     << "priority" << 0)
                                                       << BSON("_id" << 3 << "host"
                                                                     << "h3")))));

    ASSERT_OK(oldConfig.validate());
    ASSERT_OK(newConfig.validate());
    ASSERT_OK(oldConfigRefresh.validate());
    ASSERT_OK(validateConfigForReconfig(
                  &externalState, oldConfig, oldConfigRefresh, getGlobalServiceContext(), false)
                  .getStatus());
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(
                      &externalState, oldConfig, newConfig, getGlobalServiceContext(), false)
                      .getStatus());

    // Forced reconfigs also do not allow this.
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(
                      &externalState, oldConfig, newConfig, getGlobalServiceContext(), true)
                      .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForReconfig_NewConfigMustNotFlipArbiterFlag) {
    ReplicationCoordinatorExternalStateMock externalState;
    externalState.addSelf(HostAndPort("h1"));

    ReplSetConfig oldConfig;
    ReplSetConfig newConfig;
    ReplSetConfig oldConfigRefresh;

    // Three configurations, two compatible except that h2 flips the arbiterOnly flag.
    // The third, compatible with the first.
    ASSERT_OK(oldConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                 << "h1")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "h2"
                                                                    << "arbiterOnly" << false)
                                                      << BSON("_id" << 3 << "host"
                                                                    << "h3")))));

    ASSERT_OK(newConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 3 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                 << "h1")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "h2"
                                                                    << "arbiterOnly" << true)
                                                      << BSON("_id" << 3 << "host"
                                                                    << "h3")))));

    ASSERT_OK(
        oldConfigRefresh.initialize(BSON("_id"
                                         << "rs0"
                                         << "version" << 2 << "protocolVersion" << 1 << "members"
                                         << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                  << "h1")
                                                       << BSON("_id" << 2 << "host"
                                                                     << "h2"
                                                                     << "arbiterOnly" << false)
                                                       << BSON("_id" << 3 << "host"
                                                                     << "h3")))));

    ASSERT_OK(oldConfig.validate());
    ASSERT_OK(newConfig.validate());
    ASSERT_OK(oldConfigRefresh.validate());
    ASSERT_OK(validateConfigForReconfig(
                  &externalState, oldConfig, oldConfigRefresh, getGlobalServiceContext(), false)
                  .getStatus());
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(
                      &externalState, oldConfig, newConfig, getGlobalServiceContext(), false)
                      .getStatus());
    // Forced reconfigs also do not allow this.
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(
                      &externalState, oldConfig, newConfig, getGlobalServiceContext(), true)
                      .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForReconfig_HostAndIdRemappingRestricted) {
    // When reconfiguring a replica set, it is allowed to introduce (host, id) pairs
    // absent from the old config only when the hosts and ids were both individually
    // absent in the old config.

    ReplicationCoordinatorExternalStateMock externalState;
    externalState.addSelf(HostAndPort("h1"));

    ReplSetConfig oldConfig;
    ReplSetConfig legalNewConfigWithNewHostAndId;
    ReplSetConfig illegalNewConfigReusingHost;
    ReplSetConfig illegalNewConfigReusingId;

    ASSERT_OK(oldConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                 << "h1")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "h2")
                                                      << BSON("_id" << 3 << "host"
                                                                    << "h3")))));
    ASSERT_OK(oldConfig.validate());

    //
    // Here, the new config is valid because we've replaced (2, "h2") with
    // (4, "h4"), so neither the member _id or host name were reused.
    //
    ASSERT_OK(
        legalNewConfigWithNewHostAndId.initialize(BSON("_id"
                                                       << "rs0"
                                                       << "version" << 2 << "protocolVersion" << 1
                                                       << "members"
                                                       << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                                << "h1")
                                                                     << BSON("_id" << 4 << "host"
                                                                                   << "h4")
                                                                     << BSON("_id" << 3 << "host"
                                                                                   << "h3")))));
    ASSERT_OK(legalNewConfigWithNewHostAndId.validate());
    ASSERT_OK(
        validateConfigForReconfig(&externalState,
                                  oldConfig,
                                  legalNewConfigWithNewHostAndId,
                                  getGlobalServiceContext(),
                                  // Use 'force' since we're adding and removing a node atomically.
                                  true)
            .getStatus());

    //
    // Here, the new config is invalid because we've reused host name "h2" with
    // new _id 4.
    //
    ASSERT_OK(illegalNewConfigReusingHost.initialize(BSON("_id"
                                                          << "rs0"
                                                          << "version" << 2 << "protocolVersion"
                                                          << 1 << "members"
                                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                                   << "h1")
                                                                        << BSON("_id" << 4 << "host"
                                                                                      << "h2")
                                                                        << BSON("_id" << 3 << "host"
                                                                                      << "h3")))));
    ASSERT_OK(illegalNewConfigReusingHost.validate());
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(&externalState,
                                            oldConfig,
                                            illegalNewConfigReusingHost,
                                            getGlobalServiceContext(),
                                            false)
                      .getStatus());
    // Forced reconfigs also do not allow this.
    ASSERT_EQUALS(
        ErrorCodes::NewReplicaSetConfigurationIncompatible,
        validateConfigForReconfig(
            &externalState, oldConfig, illegalNewConfigReusingHost, getGlobalServiceContext(), true)
            .getStatus());
    //
    // Here, the new config is valid, because all we've changed is the name of
    // the host representing _id 2.
    //
    ASSERT_OK(illegalNewConfigReusingId.initialize(BSON("_id"
                                                        << "rs0"
                                                        << "version" << 2 << "protocolVersion" << 1
                                                        << "members"
                                                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                                 << "h1")
                                                                      << BSON("_id" << 2 << "host"
                                                                                    << "h4")
                                                                      << BSON("_id" << 3 << "host"
                                                                                    << "h3")))));
    ASSERT_OK(illegalNewConfigReusingId.validate());
    ASSERT_OK(
        validateConfigForReconfig(
            &externalState, oldConfig, illegalNewConfigReusingId, getGlobalServiceContext(), false)
            .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForReconfig_MustFindSelf) {
    // Old and new config are same except for version change; this is just testing that we can
    // find ourself in the new config.
    ReplSetConfig oldConfig;
    ASSERT_OK(oldConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                 << "h1")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "h2")
                                                      << BSON("_id" << 3 << "host"
                                                                    << "h3")))));

    ReplSetConfig newConfig;
    ASSERT_OK(newConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 2 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                 << "h1")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "h2")
                                                      << BSON("_id" << 3 << "host"
                                                                    << "h3")))));
    ReplicationCoordinatorExternalStateMock notPresentExternalState;
    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ReplicationCoordinatorExternalStateMock presentThriceExternalState;
    presentThriceExternalState.addSelf(HostAndPort("h3"));
    presentThriceExternalState.addSelf(HostAndPort("h2"));
    presentThriceExternalState.addSelf(HostAndPort("h1"));

    ASSERT_EQUALS(
        ErrorCodes::NodeNotFound,
        validateConfigForReconfig(
            &notPresentExternalState, oldConfig, newConfig, getGlobalServiceContext(), false)
            .getStatus());
    ASSERT_EQUALS(
        ErrorCodes::InvalidReplicaSetConfig,
        validateConfigForReconfig(
            &presentThriceExternalState, oldConfig, newConfig, getGlobalServiceContext(), false)
            .getStatus());
    ASSERT_EQUALS(
        1,
        unittest::assertGet(validateConfigForReconfig(
            &presentOnceExternalState, oldConfig, newConfig, getGlobalServiceContext(), false)));
    // Forced reconfigs also do not allow this.
    ASSERT_EQUALS(
        ErrorCodes::NodeNotFound,
        validateConfigForReconfig(
            &notPresentExternalState, oldConfig, newConfig, getGlobalServiceContext(), true)
            .getStatus());
    ASSERT_EQUALS(
        ErrorCodes::InvalidReplicaSetConfig,
        validateConfigForReconfig(
            &presentThriceExternalState, oldConfig, newConfig, getGlobalServiceContext(), true)
            .getStatus());
    ASSERT_EQUALS(
        1,
        unittest::assertGet(validateConfigForReconfig(
            &presentOnceExternalState, oldConfig, newConfig, getGlobalServiceContext(), true)));
}

TEST_F(ServiceContextTest, ValidateConfigForReconfig_ArbiterPriorityValueMustBeZeroOrOne) {
    ReplicationCoordinatorExternalStateMock externalState;
    externalState.addSelf(HostAndPort("h1"));

    ReplSetConfig oldConfig;
    ReplSetConfig zeroConfig;
    ReplSetConfig oneConfig;
    ReplSetConfig twoConfig;

    ASSERT_OK(oldConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                 << "h1")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "h2"
                                                                    << "arbiterOnly" << true)
                                                      << BSON("_id" << 3 << "host"
                                                                    << "h3")))));

    ASSERT_OK(zeroConfig.initialize(BSON("_id"
                                         << "rs0"
                                         << "version" << 2 << "protocolVersion" << 1 << "members"
                                         << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                  << "h1")
                                                       << BSON("_id" << 2 << "host"
                                                                     << "h2"
                                                                     << "priority" << 0
                                                                     << "arbiterOnly" << true)
                                                       << BSON("_id" << 3 << "host"
                                                                     << "h3")))));
    ASSERT_OK(oneConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 2 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                 << "h1")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "h2"
                                                                    << "priority" << 1
                                                                    << "arbiterOnly" << true)
                                                      << BSON("_id" << 3 << "host"
                                                                    << "h3")))));
    ASSERT_OK(twoConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 2 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                 << "h1")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "h2"
                                                                    << "priority" << 2
                                                                    << "arbiterOnly" << true)
                                                      << BSON("_id" << 3 << "host"
                                                                    << "h3")))));

    ASSERT_OK(oldConfig.validate());
    ASSERT_OK(zeroConfig.validate());
    ASSERT_OK(oneConfig.validate());
    ASSERT_OK(twoConfig.validate());
    ASSERT_OK(validateConfigForReconfig(
                  &externalState, oldConfig, zeroConfig, getGlobalServiceContext(), false)
                  .getStatus());
    ASSERT_OK(validateConfigForReconfig(
                  &externalState, oldConfig, oneConfig, getGlobalServiceContext(), false)
                  .getStatus());
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  validateConfigForReconfig(
                      &externalState, oldConfig, twoConfig, getGlobalServiceContext(), false)
                      .getStatus());
    // Forced reconfigs also do not allow this.
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  validateConfigForReconfig(
                      &externalState, oldConfig, twoConfig, getGlobalServiceContext(), true)
                      .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForReconfig_SelfMustEndElectable) {
    // Old and new config are same except for version change and the electability of one node;
    // this is just testing that we must be electable in the new config.
    ReplSetConfig oldConfig;
    ASSERT_OK(oldConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                 << "h1")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "h2")
                                                      << BSON("_id" << 3 << "host"
                                                                    << "h3")))));

    ReplSetConfig newConfig;
    ASSERT_OK(newConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 2 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                 << "h1")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "h2"
                                                                    << "priority" << 0)
                                                      << BSON("_id" << 3 << "host"
                                                                    << "h3")))));
    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));

    ASSERT_EQUALS(
        ErrorCodes::NodeNotElectable,
        validateConfigForReconfig(
            &presentOnceExternalState, oldConfig, newConfig, getGlobalServiceContext(), false)
            .getStatus());
    // Forced reconfig does not require electability.
    ASSERT_OK(validateConfigForReconfig(
                  &presentOnceExternalState, oldConfig, newConfig, getGlobalServiceContext(), true)
                  .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForInitiate_NewConfigInvalid) {
    // The new config is not valid due to a duplicate _id value. This tests that if the new
    // config is invalid, validateConfigForInitiate will return a status indicating what is
    // wrong with the new config.
    ReplSetConfig newConfig;
    ASSERT_OK(newConfig.initializeForInitiate(BSON("_id"
                                                   << "rs0"
                                                   << "version" << 2 << "protocolVersion" << 1
                                                   << "members"
                                                   << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                            << "h2")
                                                                 << BSON("_id" << 0 << "host"
                                                                               << "h3")))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ASSERT_EQUALS(
        ErrorCodes::BadValue,
        validateConfigForInitiate(&presentOnceExternalState, newConfig, getGlobalServiceContext())
            .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForReconfig_NewConfigInvalid) {
    // The new config is not valid due to a duplicate _id value. This tests that if the new
    // config is invalid, validateConfigForReconfig will return a status indicating what is
    // wrong with the new config.
    ReplSetConfig oldConfig;
    ASSERT_OK(oldConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "h2")))));

    ReplSetConfig newConfig;
    ASSERT_OK(newConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 2 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "h2")
                                                      << BSON("_id" << 0 << "host"
                                                                    << "h3")))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ASSERT_EQUALS(
        ErrorCodes::BadValue,
        validateConfigForReconfig(
            &presentOnceExternalState, oldConfig, newConfig, getGlobalServiceContext(), false)
            .getStatus());
    // Forced reconfigs also do not allow this.
    ASSERT_EQUALS(
        ErrorCodes::BadValue,
        validateConfigForReconfig(
            &presentOnceExternalState, oldConfig, newConfig, getGlobalServiceContext(), true)
            .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForReconfig_NewConfigWriteConcernNotSatisifiable) {
    // The new config is not valid due to an unsatisfiable write concern. This tests that if the
    // new config is invalid, validateConfigForReconfig will return a status indicating what is
    // wrong with the new config.
    ReplSetConfig oldConfig;
    ASSERT_OK(oldConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "h2")))));

    ReplSetConfig newConfig;
    ASSERT_OK(newConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "h2"))
                                        << "settings"
                                        << BSON("getLastErrorDefaults" << BSON("w" << 2)))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ASSERT_EQUALS(
        ErrorCodes::UnsatisfiableWriteConcern,
        validateConfigForReconfig(
            &presentOnceExternalState, oldConfig, newConfig, getGlobalServiceContext(), false)
            .getStatus());
    // Forced reconfigs also do not allow this.
    ASSERT_EQUALS(
        ErrorCodes::UnsatisfiableWriteConcern,
        validateConfigForReconfig(
            &presentOnceExternalState, oldConfig, newConfig, getGlobalServiceContext(), true)
            .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForStartUp_NewConfigInvalid) {
    // The new config is not valid due to a duplicate _id value. This tests that if the new
    // config is invalid, validateConfigForStartUp will return a status indicating what is wrong
    // with the new config.
    ReplSetConfig newConfig;
    ASSERT_OK(newConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 2 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "h2")
                                                      << BSON("_id" << 0 << "host"
                                                                    << "h3")))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ASSERT_EQUALS(
        ErrorCodes::BadValue,
        validateConfigForStartUp(&presentOnceExternalState, newConfig, getGlobalServiceContext())
            .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForStartUp_NewConfigValid) {
    // The new config is valid. This tests that validateConfigForStartUp will return a
    // Status::OK() indicating the validity of this configuration.
    ReplSetConfig newConfig;
    ASSERT_OK(newConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 2 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "h2"
                                                                 << "priority" << 3)
                                                      << BSON("_id" << 1 << "host"
                                                                    << "h3")))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ASSERT_OK(
        validateConfigForStartUp(&presentOnceExternalState, newConfig, getGlobalServiceContext())
            .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForStartUp_NewConfigWriteConcernNotSatisfiable) {
    // The new config contains an unsatisfiable write concern.  We don't allow these configs to be
    // created anymore, but we allow any which exist to pass and the database to start up to
    // maintain backwards compatibility.
    ReplSetConfig newConfig;
    ASSERT_OK(newConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 2 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "h2"))
                                        << "settings"
                                        << BSON("getLastErrorDefaults" << BSON("w" << 2)))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ASSERT_OK(
        validateConfigForStartUp(&presentOnceExternalState, newConfig, getGlobalServiceContext())
            .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForHeartbeatReconfig_NewConfigInvalid) {
    // The new config is not valid due to a duplicate _id value. This tests that if the new
    // config is invalid, validateConfigForHeartbeatReconfig will return a status indicating
    // what is wrong with the new config.
    ReplSetConfig newConfig;
    ASSERT_OK(newConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 2 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "h2")
                                                      << BSON("_id" << 0 << "host"
                                                                    << "h3")))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ASSERT_EQUALS(ErrorCodes::BadValue,
                  validateConfigForHeartbeatReconfig(
                      &presentOnceExternalState, newConfig, getGlobalServiceContext())
                      .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForHeartbeatReconfig_NewConfigValid) {
    // The new config is valid. This tests that validateConfigForHeartbeatReconfig will return
    // a Status::OK() indicating the validity of this config change.
    ReplSetConfig newConfig;
    ASSERT_OK(newConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 2 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "h2")
                                                      << BSON("_id" << 1 << "host"
                                                                    << "h3")))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ASSERT_OK(validateConfigForHeartbeatReconfig(
                  &presentOnceExternalState, newConfig, getGlobalServiceContext())
                  .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForHeartbeatReconfig_NewConfigWriteConcernNotSatisfiable) {
    // The new config contains an unsatisfiable write concern.  We don't allow these configs to be
    // created anymore, but we allow any which exist to be received in a heartbeat.
    ReplSetConfig newConfig;
    ASSERT_OK(newConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 2 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "h2")
                                                      << BSON("_id" << 1 << "host"
                                                                    << "h3"))
                                        << "settings"
                                        << BSON("getLastErrorDefaults" << BSON("w" << 2)))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ASSERT_OK(validateConfigForHeartbeatReconfig(
                  &presentOnceExternalState, newConfig, getGlobalServiceContext())
                  .getStatus());
}

TEST_F(ServiceContextTest, ValidateForReconfig_ForceStillNeedsValidConfig) {
    // The new config is invalid due to two nodes with the same _id value. This tests that
    // ValidateForReconfig fails with an invalid config, even if force is true.
    ReplSetConfig oldConfig;
    ASSERT_OK(oldConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "h2")
                                                      << BSON("_id" << 1 << "host"
                                                                    << "h3")))));


    ReplSetConfig newConfig;
    ASSERT_OK(newConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 2 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "h2")
                                                      << BSON("_id" << 0 << "host"
                                                                    << "h3")))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ASSERT_EQUALS(
        ErrorCodes::BadValue,
        validateConfigForReconfig(
            &presentOnceExternalState, oldConfig, newConfig, getGlobalServiceContext(), true)
            .getStatus());
}

TEST_F(ServiceContextTest, ValidateForReconfig_ForceStillNeedsSelfPresent) {
    // The new config does not contain self. This tests that ValidateForReconfig fails
    // if the member receiving it is absent from the config, even if force is true.
    ReplSetConfig oldConfig;
    ASSERT_OK(oldConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "h2")
                                                      << BSON("_id" << 1 << "host"
                                                                    << "h3")))));


    ReplSetConfig newConfig;
    ASSERT_OK(newConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version" << 2 << "protocolVersion" << 1 << "members"
                                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                 << "h3")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "h4")))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ASSERT_EQUALS(
        ErrorCodes::NodeNotFound,
        validateConfigForReconfig(
            &presentOnceExternalState, oldConfig, newConfig, getGlobalServiceContext(), true)
            .getStatus());
}

//
// Reusable member object definitions for reconfig tests below.
//
BSONObj m1 = BSON("_id" << 1 << "host"
                        << "h1");
BSONObj m2 = BSON("_id" << 2 << "host"
                        << "h2");
BSONObj m3 = BSON("_id" << 3 << "host"
                        << "h3");
BSONObj m4 = BSON("_id" << 4 << "host"
                        << "h4");
BSONObj m2_Arbiter = BSON("_id" << 2 << "host"
                                << "h2"
                                << "arbiterOnly" << true);
BSONObj m3_Arbiter = BSON("_id" << 3 << "host"
                                << "h3"
                                << "arbiterOnly" << true);
BSONObj m4_Arbiter = BSON("_id" << 4 << "host"
                                << "h4"
                                << "arbiterOnly" << true);
BSONObj m2_NonVoting = BSON("_id" << 2 << "host"
                                  << "h2"
                                  << "votes" << 0 << "priority" << 0);
BSONObj m3_NonVoting = BSON("_id" << 3 << "host"
                                  << "h3"
                                  << "votes" << 0 << "priority" << 0);
BSONObj m4_NonVoting = BSON("_id" << 4 << "host"
                                  << "h4"
                                  << "votes" << 0 << "priority" << 0);

// Test helper to initialize config more concisely.
Status initializeConfig(ReplSetConfig& cfg, std::string id, int version, BSONArray members) {
    return cfg.initialize(BSON("_id" << id << "version" << version << "protocolVersion" << 1
                                     << "members" << members));
}

// Validate reconfig between the two given member arrays and return the Status.
Status validateMemberReconfig(BSONArray oldMembers, BSONArray newMembers, BSONObj selfMember) {
    // Initialize configs.
    ReplSetConfig oldConfig, newConfig;
    ASSERT_OK(initializeConfig(oldConfig, "rs0", 1, oldMembers));
    ASSERT_OK(initializeConfig(newConfig, "rs0", 2, newMembers));
    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort(selfMember.getStringField("host")));

    // Do reconfig.
    const bool force = false;
    return validateConfigForReconfig(
               &presentOnceExternalState, oldConfig, newConfig, getGlobalServiceContext(), force)
        .getStatus();
}

TEST_F(ServiceContextTest, ValidateForReconfig_SingleNodeAdditionAllowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2);
    BSONArray newMembers = BSON_ARRAY(m1 << m2 << m3);  // add 1 voting node.
    ASSERT_OK(validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest, ValidateForReconfig_SingleNodeRemovalAllowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2 << m3);
    BSONArray newMembers = BSON_ARRAY(m1 << m2);  // remove 1 voting node.
    ASSERT_OK(validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest, ValidateForReconfig_SimultaneousAddAndRemoveDisallowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2);
    BSONArray newMembers = BSON_ARRAY(m1 << m3);  // remove node 2, add node 3.
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest, ValidateForReconfig_MultiNodeAdditionDisallowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2);
    BSONArray newMembers = BSON_ARRAY(m1 << m2 << m3 << m4);  // add 2 voting nodes.
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest, ValidateForReconfig_MultiNodeRemovalDisallowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2 << m3 << m4);
    BSONArray newMembers = BSON_ARRAY(m1 << m2);  // remove 2 voting nodes.
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest, ValidateForReconfig_MultiNodeAdditionOfNonVotingNodesAllowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2);
    BSONArray newMembers =
        BSON_ARRAY(m1 << m2 << m3_NonVoting << m4_NonVoting);  // add 2 non-voting nodes.
    ASSERT_OK(validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest, ValidateForReconfig_MultiNodeRemovalOfNonVotingNodesAllowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2 << m3_NonVoting << m4_NonVoting);
    BSONArray newMembers = BSON_ARRAY(m1 << m2);  // remove 2 non-voting nodes.
    ASSERT_OK(validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest, ValidateForReconfig_SimultaneousAddAndRemoveOfNonVotingNodesAllowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2_NonVoting);
    BSONArray newMembers = BSON_ARRAY(m1 << m3_NonVoting);  // Remove non-voter 2, add non-voter 3.
    ASSERT_OK(validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest, ValidateForReconfig_SingleNodeAdditionOfArbitersAllowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2);
    BSONArray newMembers = BSON_ARRAY(m1 << m2 << m3_Arbiter);  // add 1 arbiter.
    ASSERT_OK(validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest, ValidateForReconfig_SimultaneousAddAndRemoveOfArbitersDisallowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2_Arbiter);
    BSONArray newMembers = BSON_ARRAY(m1 << m3_Arbiter);  // remove node 2, add node 3.
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest, ValidateForReconfig_MultiNodeAdditionOfArbitersDisallowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2);
    BSONArray newMembers = BSON_ARRAY(m1 << m2 << m3_Arbiter << m4_Arbiter);  // add two arbiters.
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  validateMemberReconfig(oldMembers, newMembers, m1));
}

}  // namespace
}  // namespace repl
}  // namespace mongo
