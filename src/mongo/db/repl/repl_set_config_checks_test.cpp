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
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/death_test.h"
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
    OID newReplSetId = OID::gen();
    config = ReplSetConfig::parseForInitiate(BSON("_id"
                                                  << "rs0"
                                                  << "version" << 2 << "protocolVersion" << 1
                                                  << "members"
                                                  << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                           << "h1"))),
                                             newReplSetId);
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForInitiate(&rses, config, getServiceContext()).getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForInitiate_TermIsAlwaysInitialTerm) {
    ReplicationCoordinatorExternalStateMock rses;
    rses.addSelf(HostAndPort("h1"));

    OID newReplSetId = OID::gen();
    auto config = ReplSetConfig::parseForInitiate(BSON("_id"
                                                       << "rs0"
                                                       << "version" << 1 << "term"
                                                       << (OpTime::kInitialTerm + 1)
                                                       << "protocolVersion" << 1 << "members"
                                                       << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                                << "h1"))),
                                                  newReplSetId);
    ASSERT_OK(validateConfigForInitiate(&rses, config, getServiceContext()).getStatus());
    ASSERT_EQUALS(config.getConfigTerm(), OpTime::kInitialTerm);
}

TEST_F(ServiceContextTest, ValidateConfigForInitiate_memberId) {
    ReplicationCoordinatorExternalStateMock rses;
    rses.addSelf(HostAndPort("h1"));

    // Config with Member id > 255.
    OID newReplSetId = OID::gen();
    auto validConfig =
        ReplSetConfig::parseForInitiate(BSON("_id"
                                             << "rs0"
                                             << "version" << 1 << "protocolVersion" << 1
                                             << "members"
                                             << BSON_ARRAY(BSON("_id" << 256 << "host"
                                                                      << "h1"))),
                                        newReplSetId);
    ASSERT_OK(validateConfigForInitiate(&rses, validConfig, getGlobalServiceContext()).getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForInitiate_MustFindSelf) {
    ReplSetConfig config;
    OID newReplSetId = OID::gen();
    config = ReplSetConfig::parseForInitiate(BSON("_id"
                                                  << "rs0"
                                                  << "version" << 1 << "protocolVersion" << 1
                                                  << "members"
                                                  << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                           << "h1")
                                                                << BSON("_id" << 2 << "host"
                                                                              << "h2")
                                                                << BSON("_id" << 3 << "host"
                                                                              << "h3"))),
                                             newReplSetId);
    ReplicationCoordinatorExternalStateMock notPresentExternalState;
    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ReplicationCoordinatorExternalStateMock presentTwiceExternalState;
    presentTwiceExternalState.addSelf(HostAndPort("h3"));
    presentTwiceExternalState.addSelf(HostAndPort("h1"));

    ASSERT_EQUALS(ErrorCodes::NodeNotFound,
                  validateConfigForInitiate(&notPresentExternalState, config, getServiceContext())
                      .getStatus());
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  validateConfigForInitiate(&presentTwiceExternalState, config, getServiceContext())
                      .getStatus());
    ASSERT_EQUALS(1,
                  unittest::assertGet(validateConfigForInitiate(
                      &presentOnceExternalState, config, getServiceContext())));
}

TEST_F(ServiceContextTest, ValidateConfigForInitiate_SelfMustBeElectable) {
    ReplSetConfig config;
    OID newReplSetId = OID::gen();
    config = ReplSetConfig::parseForInitiate(BSON("_id"
                                                  << "rs0"
                                                  << "version" << 1 << "protocolVersion" << 1
                                                  << "members"
                                                  << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                           << "h1")
                                                                << BSON("_id" << 2 << "host"
                                                                              << "h2"
                                                                              << "priority" << 0)
                                                                << BSON("_id" << 3 << "host"
                                                                              << "h3"))),
                                             newReplSetId);
    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));

    ASSERT_EQUALS(ErrorCodes::NodeNotElectable,
                  validateConfigForInitiate(&presentOnceExternalState, config, getServiceContext())
                      .getStatus());
}

DEATH_TEST_REGEX_F(ServiceContextTest,
                   ValidateConfigForInitiate_NonDefaultGetLastErrorDefaults,
                   "Fatal assertion.*5624101") {
    ReplSetConfig config;
    OID newReplSetId = OID::gen();
    config =
        ReplSetConfig::parseForInitiate(BSON("_id"
                                             << "rs0"
                                             << "version" << 1 << "protocolVersion" << 1
                                             << "members"
                                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                      << "h1"))
                                             << "settings"
                                             << BSON("getLastErrorDefaults" << BSON("w" << 2))),
                                        newReplSetId);
    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    validateConfigForInitiate(&presentOnceExternalState, config, getServiceContext()).getStatus();
}

TEST_F(ServiceContextTest, ValidateConfigForInitiate_ArbiterPriorityMustBeZeroOrOne) {
    ReplSetConfig zeroConfig;
    ReplSetConfig oneConfig;
    ReplSetConfig twoConfig;
    OID newReplSetId = OID::gen();
    zeroConfig = ReplSetConfig::parseForInitiate(
        BSON("_id"
             << "rs0"
             << "version" << 1 << "protocolVersion" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                      << "h1")
                           << BSON("_id" << 2 << "host"
                                         << "h2"
                                         << "priority" << 0 << "arbiterOnly" << true)
                           << BSON("_id" << 3 << "host"
                                         << "h3"))),
        newReplSetId);

    oneConfig = ReplSetConfig::parseForInitiate(
        BSON("_id"
             << "rs0"
             << "version" << 1 << "protocolVersion" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                      << "h1")
                           << BSON("_id" << 2 << "host"
                                         << "h2"
                                         << "priority" << 1 << "arbiterOnly" << true)
                           << BSON("_id" << 3 << "host"
                                         << "h3"))),
        newReplSetId);

    twoConfig = ReplSetConfig::parseForInitiate(
        BSON("_id"
             << "rs0"
             << "version" << 1 << "protocolVersion" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                      << "h1")
                           << BSON("_id" << 2 << "host"
                                         << "h2"
                                         << "priority" << 2 << "arbiterOnly" << true)
                           << BSON("_id" << 3 << "host"
                                         << "h3"))),
        newReplSetId);
    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h1"));

    ASSERT_OK(validateConfigForInitiate(&presentOnceExternalState, zeroConfig, getServiceContext())
                  .getStatus());
    ASSERT_OK(validateConfigForInitiate(&presentOnceExternalState, oneConfig, getServiceContext())
                  .getStatus());
    ASSERT_EQUALS(
        ErrorCodes::InvalidReplicaSetConfig,
        validateConfigForInitiate(&presentOnceExternalState, twoConfig, getServiceContext())
            .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForInitiate_NewlyAddedFieldNotAllowed) {
    ReplSetConfig firstNewlyAdded;
    ReplSetConfig lastNewlyAdded;
    OID newReplSetId = OID::gen();
    firstNewlyAdded =
        ReplSetConfig::parseForInitiate(BSON("_id"
                                             << "rs0"
                                             << "version" << 1 << "protocolVersion" << 1
                                             << "members"
                                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                      << "newly_added_h1"
                                                                      << "newlyAdded" << true)
                                                           << BSON("_id" << 2 << "host"
                                                                         << "h2")
                                                           << BSON("_id" << 3 << "host"
                                                                         << "h3"))),
                                        newReplSetId);

    lastNewlyAdded =
        ReplSetConfig::parseForInitiate(BSON("_id"
                                             << "rs0"
                                             << "version" << 1 << "protocolVersion" << 1
                                             << "members"
                                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                      << "h1")
                                                           << BSON("_id" << 2 << "host"
                                                                         << "h2")
                                                           << BSON("_id" << 3 << "host"
                                                                         << "newly_added_h3"
                                                                         << "newlyAdded" << true))),
                                        newReplSetId);

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h1"));

    auto status =
        validateConfigForInitiate(&presentOnceExternalState, firstNewlyAdded, getServiceContext())
            .getStatus();
    ASSERT_EQUALS(status, ErrorCodes::InvalidReplicaSetConfig);
    ASSERT_TRUE(status.reason().find("newly_added_h1") != std::string::npos);
    status =
        validateConfigForInitiate(&presentOnceExternalState, lastNewlyAdded, getServiceContext())
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
    oldConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 1 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "h2")
                                                        << BSON("_id" << 3 << "host"
                                                                      << "h3"))));

    newConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 3 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "h2")
                                                        << BSON("_id" << 3 << "host"
                                                                      << "h3"))));

    ASSERT_OK(oldConfig.validate());
    ASSERT_OK(newConfig.validate());

    // Can reconfig from old to new.
    ASSERT_OK(validateConfigForReconfig(oldConfig, newConfig, false, false));


    // Cannot reconfig from old to old (versions must be different).
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(oldConfig, oldConfig, false, false));

    // Cannot reconfig from new to old (versions must increase).
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(newConfig, oldConfig, false, false));
}

TEST_F(ServiceContextTest, ValidateConfigForReconfig_memberId) {
    ReplicationCoordinatorExternalStateMock externalState;
    externalState.addSelf(HostAndPort("h1"));

    ReplSetConfig oldConfig;
    ReplSetConfig newConfig;

    // Case 1: Add a new node with member id > 255.
    oldConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 1 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1"))));
    newConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 2 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 256 << "host"
                                                                      << "h2"))));
    ASSERT_OK(oldConfig.validate());
    ASSERT_OK(newConfig.validate());
    ASSERT_OK(validateConfigForReconfig(oldConfig, newConfig, false, false));

    // Case 2: Change the member config setting for the existing member with member id > 255.
    oldConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 1 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 256 << "host"
                                                                      << "h2"))));
    newConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 2 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 256 << "host"
                                                                      << "h2"
                                                                      << "priority" << 0))));
    ASSERT_OK(oldConfig.validate());
    ASSERT_OK(newConfig.validate());
    ASSERT_OK(validateConfigForReconfig(oldConfig, newConfig, false, false));
}


TEST_F(ServiceContextTest, ValidateConfigForReconfig_NewConfigMustNotChangeSetName) {
    ReplicationCoordinatorExternalStateMock externalState;
    externalState.addSelf(HostAndPort("h1"));

    ReplSetConfig oldConfig;
    ReplSetConfig newConfig;

    // Two configurations, compatible except for set name.
    oldConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 1 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "h2")
                                                        << BSON("_id" << 3 << "host"
                                                                      << "h3"))));

    newConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs1"
                                          << "version" << 3 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "h2")
                                                        << BSON("_id" << 3 << "host"
                                                                      << "h3"))));

    ASSERT_OK(oldConfig.validate());
    ASSERT_OK(newConfig.validate());
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(oldConfig, newConfig, false, false));
    // Forced reconfigs also do not allow this.
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(newConfig, oldConfig, true, false));
}

TEST_F(ServiceContextTest, ValidateConfigForReconfig_NewConfigMustNotChangeSetId) {
    ReplicationCoordinatorExternalStateMock externalState;
    externalState.addSelf(HostAndPort("h1"));

    ReplSetConfig oldConfig;
    ReplSetConfig newConfig;

    // Two configurations, compatible except for set ID.
    oldConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 1 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "h2")
                                                        << BSON("_id" << 3 << "host"
                                                                      << "h3"))
                                          << "settings" << BSON("replicaSetId" << OID::gen())));

    newConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 3 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "h2")
                                                        << BSON("_id" << 3 << "host"
                                                                      << "h3"))
                                          << "settings" << BSON("replicaSetId" << OID::gen())));

    ASSERT_OK(oldConfig.validate());
    ASSERT_OK(newConfig.validate());
    const auto status = validateConfigForReconfig(oldConfig, newConfig, false, false);
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible, status);
    ASSERT_STRING_CONTAINS(status.reason(), "New and old configurations differ in replica set ID");

    // Forced reconfigs also do not allow this.
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(newConfig, oldConfig, true, false));
}

TEST_F(ServiceContextTest, ValidateConfigForReconfig_NewConfigMustNotFlipBuildIndexesFlag) {
    ReplicationCoordinatorExternalStateMock externalState;
    externalState.addSelf(HostAndPort("h1"));

    ReplSetConfig oldConfig;
    ReplSetConfig newConfig;
    ReplSetConfig oldConfigRefresh;

    // Three configurations, two compatible except that h2 flips the buildIndex flag.
    // The third, compatible with the first.
    oldConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 1 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "h2"
                                                                      << "buildIndexes" << false
                                                                      << "priority" << 0)
                                                        << BSON("_id" << 3 << "host"
                                                                      << "h3"))));

    newConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 3 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "h2"
                                                                      << "buildIndexes" << true
                                                                      << "priority" << 0)
                                                        << BSON("_id" << 3 << "host"
                                                                      << "h3"))));


    oldConfigRefresh =
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 2 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                           << "h1")
                                                << BSON("_id" << 2 << "host"
                                                              << "h2"
                                                              << "buildIndexes" << false
                                                              << "priority" << 0)
                                                << BSON("_id" << 3 << "host"
                                                              << "h3"))));

    ASSERT_OK(oldConfig.validate());
    ASSERT_OK(newConfig.validate());
    ASSERT_OK(oldConfigRefresh.validate());
    ASSERT_OK(validateConfigForReconfig(oldConfig, oldConfigRefresh, false, false));
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(oldConfig, newConfig, false, false));

    // Forced reconfigs also do not allow this.
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(oldConfig, newConfig, true, false));
}

TEST_F(ServiceContextTest, ValidateConfigForReconfig_NewConfigMustNotFlipArbiterFlag) {
    ReplicationCoordinatorExternalStateMock externalState;
    externalState.addSelf(HostAndPort("h1"));

    ReplSetConfig oldConfig;
    ReplSetConfig newConfig;
    ReplSetConfig oldConfigRefresh;

    // Three configurations, two compatible except that h2 flips the arbiterOnly flag.
    // The third, compatible with the first.
    oldConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 1 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "h2"
                                                                      << "arbiterOnly" << false)
                                                        << BSON("_id" << 3 << "host"
                                                                      << "h3"))));

    newConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 3 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "h2"
                                                                      << "arbiterOnly" << true)
                                                        << BSON("_id" << 3 << "host"
                                                                      << "h3"))));


    oldConfigRefresh =
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 2 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                           << "h1")
                                                << BSON("_id" << 2 << "host"
                                                              << "h2"
                                                              << "arbiterOnly" << false)
                                                << BSON("_id" << 3 << "host"
                                                              << "h3"))));

    ASSERT_OK(oldConfig.validate());
    ASSERT_OK(newConfig.validate());
    ASSERT_OK(oldConfigRefresh.validate());
    ASSERT_OK(validateConfigForReconfig(oldConfig, oldConfigRefresh, false, false));
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(oldConfig, newConfig, false, false));
    // Forced reconfigs also do not allow this.
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(oldConfig, newConfig, true, false));
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

    oldConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 1 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "h2")
                                                        << BSON("_id" << 3 << "host"
                                                                      << "h3"))));
    ASSERT_OK(oldConfig.validate());

    //
    // Here, the new config is valid because we've replaced (2, "h2") with
    // (4, "h4"), so neither the member _id or host name were reused.
    //

    legalNewConfigWithNewHostAndId =
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 2 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                           << "h1")
                                                << BSON("_id" << 4 << "host"
                                                              << "h4")
                                                << BSON("_id" << 3 << "host"
                                                              << "h3"))));
    ASSERT_OK(legalNewConfigWithNewHostAndId.validate());
    ASSERT_OK(
        validateConfigForReconfig(oldConfig,
                                  legalNewConfigWithNewHostAndId,
                                  // Use 'force' since we're adding and removing a node atomically.
                                  true,
                                  false));

    //
    // Here, the new config is invalid because we've reused host name "h2" with
    // new _id 4.
    //
    illegalNewConfigReusingHost =
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 2 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                           << "h1")
                                                << BSON("_id" << 4 << "host"
                                                              << "h2")
                                                << BSON("_id" << 3 << "host"
                                                              << "h3"))));
    ASSERT_OK(illegalNewConfigReusingHost.validate());
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(oldConfig, illegalNewConfigReusingHost, false, false));
    // Forced reconfigs also do not allow this.
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  validateConfigForReconfig(oldConfig, illegalNewConfigReusingHost, true, false));
    //
    // Here, the new config is valid, because all we've changed is the name of
    // the host representing _id 2.
    //
    illegalNewConfigReusingId =
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 2 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                           << "h1")
                                                << BSON("_id" << 2 << "host"
                                                              << "h4")
                                                << BSON("_id" << 3 << "host"
                                                              << "h3"))));
    ASSERT_OK(illegalNewConfigReusingId.validate());
    ASSERT_OK(validateConfigForReconfig(oldConfig, illegalNewConfigReusingId, false, false));
}

TEST_F(ServiceContextTest, ValidateConfigForReconfig_ArbiterPriorityValueMustBeZeroOrOne) {
    ReplicationCoordinatorExternalStateMock externalState;
    externalState.addSelf(HostAndPort("h1"));

    ReplSetConfig oldConfig;
    ReplSetConfig zeroConfig;
    ReplSetConfig oneConfig;
    ReplSetConfig twoConfig;

    oldConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 1 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "h2"
                                                                      << "arbiterOnly" << true)
                                                        << BSON("_id" << 3 << "host"
                                                                      << "h3"))));

    zeroConfig = ReplSetConfig::parse(BSON("_id"
                                           << "rs0"
                                           << "version" << 2 << "protocolVersion" << 1 << "members"
                                           << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                    << "h1")
                                                         << BSON("_id" << 2 << "host"
                                                                       << "h2"
                                                                       << "priority" << 0
                                                                       << "arbiterOnly" << true)
                                                         << BSON("_id" << 3 << "host"
                                                                       << "h3"))));
    oneConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 2 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "h2"
                                                                      << "priority" << 1
                                                                      << "arbiterOnly" << true)
                                                        << BSON("_id" << 3 << "host"
                                                                      << "h3"))));
    twoConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 2 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "h2"
                                                                      << "priority" << 2
                                                                      << "arbiterOnly" << true)
                                                        << BSON("_id" << 3 << "host"
                                                                      << "h3"))));

    ASSERT_OK(oldConfig.validate());
    ASSERT_OK(zeroConfig.validate());
    ASSERT_OK(oneConfig.validate());
    ASSERT_OK(twoConfig.validate());
    ASSERT_OK(validateConfigForReconfig(oldConfig, zeroConfig, false, false));
    ASSERT_OK(validateConfigForReconfig(oldConfig, oneConfig, false, false));
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  validateConfigForReconfig(oldConfig, twoConfig, false, false));
    // Forced reconfigs also do not allow this.
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  validateConfigForReconfig(oldConfig, twoConfig, true, false));
}

TEST_F(ServiceContextTest, ValidateConfigForInitiate_NewConfigInvalid) {
    // The new config is not valid due to a duplicate _id value. This tests that if the new
    // config is invalid, validateConfigForInitiate will return a status indicating what is
    // wrong with the new config.
    ReplSetConfig newConfig;
    OID newReplSetId = OID::gen();
    newConfig = ReplSetConfig::parseForInitiate(BSON("_id"
                                                     << "rs0"
                                                     << "version" << 2 << "protocolVersion" << 1
                                                     << "members"
                                                     << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                              << "h2")
                                                                   << BSON("_id" << 0 << "host"
                                                                                 << "h3"))),
                                                newReplSetId);

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ASSERT_EQUALS(
        ErrorCodes::BadValue,
        validateConfigForInitiate(&presentOnceExternalState, newConfig, getServiceContext())
            .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForReconfig_NewConfigInvalid) {
    // The new config is not valid due to a duplicate _id value. This tests that if the new
    // config is invalid, validateConfigForReconfig will return a status indicating what is
    // wrong with the new config.
    ReplSetConfig oldConfig;
    oldConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 1 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                   << "h2"))));

    ReplSetConfig newConfig;
    newConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 2 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                   << "h2")
                                                        << BSON("_id" << 0 << "host"
                                                                      << "h3"))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ASSERT_EQUALS(ErrorCodes::BadValue,
                  validateConfigForReconfig(oldConfig, newConfig, false, false));
    // Forced reconfigs also do not allow this.
    ASSERT_EQUALS(ErrorCodes::BadValue,
                  validateConfigForReconfig(oldConfig, newConfig, true, false));
}

TEST_F(ServiceContextTest, ValidateConfigForReconfig_NonDefaultGetLastErrorDefaults) {
    // The new config is not valid due to an unsatisfiable write concern. This tests that if the
    // new config is invalid, validateConfigForReconfig will return a status indicating what is
    // wrong with the new config.
    ReplSetConfig oldConfig;
    oldConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 1 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                   << "h2"))));

    ReplSetConfig newConfig;
    newConfig =
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 1 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "h2"))
                                  << "settings" << BSON("getLastErrorDefaults" << BSON("w" << 2))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ASSERT_THROWS_CODE(
        validateConfigForReconfig(oldConfig, newConfig, false, false), AssertionException, 5624102);
    // Forced reconfigs also do not allow this.
    ASSERT_THROWS_CODE(
        validateConfigForReconfig(oldConfig, newConfig, true, false), AssertionException, 5624102);
}

TEST_F(ServiceContextTest, ValidateConfigForStartUp_NewConfigInvalid) {
    // The new config is not valid due to a duplicate _id value. This tests that if the new
    // config is invalid, validateConfigForStartUp will return a status indicating what is wrong
    // with the new config.
    ReplSetConfig newConfig;
    newConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 2 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                   << "h2")
                                                        << BSON("_id" << 0 << "host"
                                                                      << "h3"))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ASSERT_EQUALS(
        ErrorCodes::BadValue,
        validateConfigForStartUp(&presentOnceExternalState, newConfig, getServiceContext())
            .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForStartUp_NewConfigValid) {
    // The new config is valid. This tests that validateConfigForStartUp will return a
    // Status::OK() indicating the validity of this configuration.
    ReplSetConfig newConfig;
    newConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 2 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                   << "h2"
                                                                   << "priority" << 3)
                                                        << BSON("_id" << 1 << "host"
                                                                      << "h3"))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ASSERT_OK(validateConfigForStartUp(&presentOnceExternalState, newConfig, getServiceContext())
                  .getStatus());
}

DEATH_TEST_REGEX_F(ServiceContextTest,
                   ValidateConfigForStartUp_NewConfigNonDefaultGetLastErrorDefaults,
                   "Fatal assertion.*5624100") {
    // The new config contains an unsatisfiable write concern.  We don't allow these configs to be
    // created anymore, but we allow any which exist to pass and the database to start up to
    // maintain backwards compatibility.
    ReplSetConfig newConfig;
    newConfig =
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 2 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "h2"))
                                  << "settings" << BSON("getLastErrorDefaults" << BSON("w" << 2))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    validateConfigForStartUp(&presentOnceExternalState, newConfig, getServiceContext()).getStatus();
}

TEST_F(ServiceContextTest, ValidateConfigForHeartbeatReconfig_NewConfigInvalid) {
    // The new config is not valid due to a duplicate _id value. This tests that if the new
    // config is invalid, validateConfigForHeartbeatReconfig will return a status indicating
    // what is wrong with the new config.
    ReplSetConfig newConfig;
    newConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 2 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                   << "h2")
                                                        << BSON("_id" << 0 << "host"
                                                                      << "h3"))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ASSERT_EQUALS(ErrorCodes::BadValue,
                  validateConfigForHeartbeatReconfig(
                      &presentOnceExternalState, newConfig, HostAndPort(), getServiceContext())
                      .getStatus());
}

TEST_F(ServiceContextTest, ValidateConfigForHeartbeatReconfig_NewConfigValid) {
    // The new config is valid. This tests that validateConfigForHeartbeatReconfig will return
    // a Status::OK() indicating the validity of this config change.
    ReplSetConfig newConfig;
    newConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 2 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                   << "h2")
                                                        << BSON("_id" << 1 << "host"
                                                                      << "h3"))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ASSERT_OK(validateConfigForHeartbeatReconfig(
                  &presentOnceExternalState, newConfig, HostAndPort(), getServiceContext())
                  .getStatus());
}

DEATH_TEST_REGEX_F(ServiceContextTest,
                   ValidateConfigForHeartbeatReconfig_NonDefaultGetLastErrorDefaults,
                   "Tripwire assertion.*5624103") {
    // The new config contains an unsatisfiable write concern.  We don't allow these configs to be
    // created anymore, but we allow any which exist to be received in a heartbeat.
    ReplSetConfig newConfig;
    newConfig =
        ReplSetConfig::parse(BSON("_id"
                                  << "rs0"
                                  << "version" << 2 << "protocolVersion" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                           << "h2")
                                                << BSON("_id" << 1 << "host"
                                                              << "h3"))
                                  << "settings" << BSON("getLastErrorDefaults" << BSON("w" << 2))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ASSERT_THROWS_CODE(validateConfigForHeartbeatReconfig(
                           &presentOnceExternalState, newConfig, HostAndPort(), getServiceContext())
                           .getStatus(),
                       AssertionException,
                       5624103);
}

TEST_F(ServiceContextTest, ValidateForReconfig_ForceStillNeedsValidConfig) {
    // The new config is invalid due to two nodes with the same _id value. This tests that
    // ValidateForReconfig fails with an invalid config, even if force is true.
    ReplSetConfig oldConfig;
    oldConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 1 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                   << "h2")
                                                        << BSON("_id" << 1 << "host"
                                                                      << "h3"))));


    ReplSetConfig newConfig;
    newConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 2 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                   << "h2")
                                                        << BSON("_id" << 0 << "host"
                                                                      << "h3"))));

    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ASSERT_EQUALS(ErrorCodes::BadValue,
                  validateConfigForReconfig(oldConfig, newConfig, true, false));
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
BSONObj m2_NewlyAdded = BSON("_id" << 2 << "host"
                                   << "h2"
                                   << "newlyAdded" << true << "votes" << 1);
BSONObj m3_NewlyAdded = BSON("_id" << 3 << "host"
                                   << "h3"
                                   << "newlyAdded" << true << "votes" << 1);
BSONObj m4_NewlyAdded = BSON("_id" << 4 << "host"
                                   << "h4"
                                   << "newlyAdded" << true << "votes" << 1);

// Test helper to initialize config more concisely.
ReplSetConfig initializeConfig(std::string id, int version, BSONArray members) {
    return ReplSetConfig::parse(BSON("_id" << id << "version" << version << "protocolVersion" << 1
                                           << "members" << members));
}

// Validate reconfig between the two given member arrays and return the Status.
Status validateMemberReconfig(BSONArray oldMembers, BSONArray newMembers, BSONObj selfMember) {
    // Initialize configs.
    auto oldConfig = initializeConfig("rs0", 1, oldMembers);
    auto newConfig = initializeConfig("rs0", 2, newMembers);
    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort(selfMember.getStringField("host")));

    // Do reconfig.
    const bool force = false;
    const bool allowSplitHorizonIP = false;
    return validateConfigForReconfig(oldConfig, newConfig, force, allowSplitHorizonIP);
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
    RAIIServerParameterControllerForTest controller{"allowMultipleArbiters", true};
    BSONArray oldMembers = BSON_ARRAY(m1 << m2);
    BSONArray newMembers = BSON_ARRAY(m1 << m2 << m3_Arbiter << m4_Arbiter);  // add two arbiters.
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest, ValidateForReconfig_SingleNodeAdditionOfNewlyAddedAllowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2);
    BSONArray newMembers = BSON_ARRAY(m1 << m2 << m3_NewlyAdded);  // add 1 'newlyAdded' node.
    ASSERT_OK(validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest, ValidateForReconfig_MultiNodeAdditionOfNewlyAddedAllowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2);
    BSONArray newMembers =
        BSON_ARRAY(m1 << m2 << m3_NewlyAdded << m4_NewlyAdded);  // add 2 'newlyAdded' nodes.
    ASSERT_OK(validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest, ValidateForReconfig_MultiNodeRemovalOfNewlyAddedAllowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2_NewlyAdded << m3_NewlyAdded);
    BSONArray newMembers = BSON_ARRAY(m1);  // Remove 2 'newlyAdded' nodes.
    ASSERT_OK(validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest, ValidateForReconfig_SimultaneousAddAndRemoveOfNewlyAddedAllowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2_NewlyAdded);
    BSONArray newMembers =
        BSON_ARRAY(m1 << m3_NewlyAdded);  // Remove 'newlyAdded' 2, add 'newlyAdded' 3.
    ASSERT_OK(validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest, ValidateForReconfig_SingleAutoReconfigAllowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2_NewlyAdded);
    BSONArray newMembers = BSON_ARRAY(m1 << m2);  // Remove 'newlyAdded' 2, add voting node 2.
    ASSERT_OK(validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest, ValidateForReconfig_MultiAutoReconfigDisallowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2_NewlyAdded << m3_NewlyAdded);
    BSONArray newMembers =
        BSON_ARRAY(m1 << m2 << m3);  // Remove 'newlyAdded' 2 & 3, add voting node 2 & 3.
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest,
       ValidateForReconfig_SimultaneousAutoReconfigAndAdditionOfVoterNodeDisallowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2_NewlyAdded);
    BSONArray newMembers =
        BSON_ARRAY(m1 << m2 << m3);  // Remove 'newlyAdded' 2, add voting node 2 & 3.
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest,
       ValidateForReconfig_SimultaneousAutoReconfigAndRemovalOfVoterNodeDisallowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2_NewlyAdded << m3);
    BSONArray newMembers =
        BSON_ARRAY(m1 << m2);  // Remove 'newlyAdded' 2 and voter node 3, add voting node 2.
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest,
       ValidateForReconfig_SimultaneousAutoReconfigAndAdditionOfNewlyAddedAllowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2_NewlyAdded);
    BSONArray newMembers = BSON_ARRAY(
        m1 << m2 << m3_NewlyAdded);  // Remove 'newlyAdded' 2, add voting node 2 & 'newlyAdded' 3.
    ASSERT_OK(validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest,
       ValidateForReconfig_SimultaneousAutoReconfigAndRemovalOfNewlyAddedAllowed) {
    BSONArray oldMembers = BSON_ARRAY(m1 << m2_NewlyAdded << m3_NewlyAdded);
    BSONArray newMembers = BSON_ARRAY(m1 << m2);  // Remove 'newlyAdded' 2 & 3, add voting node 2.
    ASSERT_OK(validateMemberReconfig(oldMembers, newMembers, m1));
}

TEST_F(ServiceContextTest, SameConfigContents) {
    ReplSetConfig configA;
    ReplSetConfig configB;
    auto members1 = BSON_ARRAY(BSON("_id" << 1 << "host"
                                          << "h1"));
    auto members2 = BSON_ARRAY(BSON("_id" << 1 << "host"
                                          << "h1")
                               << BSON("_id" << 2 << "host"
                                             << "2"));

    // Same contents with different version.
    configA = ReplSetConfig::parse(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "term" << 1 << "protocolVersion" << 1
                                        << "members" << members1));
    configB = ReplSetConfig::parse(BSON("_id"
                                        << "rs0"
                                        << "version" << 2 << "term" << 1 << "protocolVersion" << 1
                                        << "members" << members1));
    ASSERT(sameConfigContents(configA, configB));

    // Same contents with different term.
    configA = ReplSetConfig::parse(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "term" << 1 << "protocolVersion" << 1
                                        << "members" << members1));
    configB = ReplSetConfig::parse(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "term" << 2 << "protocolVersion" << 1
                                        << "members" << members1));
    ASSERT(sameConfigContents(configA, configB));

    // Same contents with different term and version.
    configA = ReplSetConfig::parse(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "term" << 1 << "protocolVersion" << 1
                                        << "members" << members1));
    configB = ReplSetConfig::parse(BSON("_id"
                                        << "rs0"
                                        << "version" << 2 << "term" << 2 << "protocolVersion" << 1
                                        << "members" << members1));
    ASSERT(sameConfigContents(configA, configB));

    // Different config settings.
    configA = ReplSetConfig::parse(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "term" << 1 << "protocolVersion" << 1
                                        << "members" << members1 << "settings"
                                        << BSON("electionTimeoutMillis" << 1000)));
    configB = ReplSetConfig::parse(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "term" << 1 << "protocolVersion" << 1
                                        << "members" << members1 << "settings"
                                        << BSON("electionTimeoutMillis" << 2000)));
    ASSERT_FALSE(sameConfigContents(configA, configB));

    // Different members.
    configA = ReplSetConfig::parse(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "term" << 1 << "protocolVersion" << 1
                                        << "members" << members1 << "settings"
                                        << BSON("electionTimeoutMillis" << 1000)));
    configB = ReplSetConfig::parse(BSON("_id"
                                        << "rs0"
                                        << "version" << 1 << "term" << 1 << "protocolVersion" << 1
                                        << "members" << members2 << "settings"
                                        << BSON("electionTimeoutMillis" << 2000)));
    ASSERT_FALSE(sameConfigContents(configA, configB));
}

TEST_F(ServiceContextTest, FindSelfInConfig) {
    // We must be able to find ourself in the new config.
    ReplSetConfig newConfig;
    newConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 2 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "h2")
                                                        << BSON("_id" << 3 << "host"
                                                                      << "h3"))));
    ReplicationCoordinatorExternalStateMock notPresentExternalState;
    ReplicationCoordinatorExternalStateMock presentOnceExternalState;
    presentOnceExternalState.addSelf(HostAndPort("h2"));
    ReplicationCoordinatorExternalStateMock presentThriceExternalState;
    presentThriceExternalState.addSelf(HostAndPort("h3"));
    presentThriceExternalState.addSelf(HostAndPort("h2"));
    presentThriceExternalState.addSelf(HostAndPort("h1"));

    // Test 'findSelfInConfig'.
    ASSERT_EQUALS(
        ErrorCodes::NodeNotFound,
        findSelfInConfig(&notPresentExternalState, newConfig, getServiceContext()).getStatus());
    ASSERT_EQUALS(
        ErrorCodes::InvalidReplicaSetConfig,
        findSelfInConfig(&presentThriceExternalState, newConfig, getServiceContext()).getStatus());
    ASSERT_EQUALS(1,
                  unittest::assertGet(
                      findSelfInConfig(&presentOnceExternalState, newConfig, getServiceContext())));

    // The same rules apply to 'findSelfInConfigIfElectable'.
    ASSERT_EQUALS(
        ErrorCodes::NodeNotFound,
        findSelfInConfigIfElectable(&notPresentExternalState, newConfig, getServiceContext())
            .getStatus());
    ASSERT_EQUALS(
        ErrorCodes::InvalidReplicaSetConfig,
        findSelfInConfigIfElectable(&presentThriceExternalState, newConfig, getServiceContext())
            .getStatus());
    ASSERT_EQUALS(1,
                  unittest::assertGet(findSelfInConfigIfElectable(
                      &presentOnceExternalState, newConfig, getServiceContext())));

    // We must be electable in the new config.
    newConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 2 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "h2"
                                                                      << "priority" << 0)
                                                        << BSON("_id" << 3 << "host"
                                                                      << "h3"))));

    ASSERT_EQUALS(
        ErrorCodes::NodeNotElectable,
        findSelfInConfigIfElectable(&presentOnceExternalState, newConfig, getServiceContext())
            .getStatus());
}

TEST_F(ServiceContextTest, FindSelfInConfigFastAndSlow) {
    ReplSetConfig newConfig;
    newConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 2 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "h2")
                                                        << BSON("_id" << 3 << "host"
                                                                      << "h3"))));

    {
        // Present once only on the slow path, but fast enough to be found
        ReplicationCoordinatorExternalStateMock presentOnceExternalState;
        presentOnceExternalState.addSelfSlow(HostAndPort("h2"), Seconds(29));
        ASSERT_EQUALS(1,
                      unittest::assertGet(findSelfInConfig(
                          &presentOnceExternalState, newConfig, getServiceContext())));
    }

    {
        // Present twice only on the slow path, but fast enough to be found both times.
        ReplicationCoordinatorExternalStateMock presentTwiceExternalState;
        presentTwiceExternalState.addSelfSlow(HostAndPort("h2"), Seconds(29));
        presentTwiceExternalState.addSelfSlow(HostAndPort("h3"), Seconds(29));
        ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                      findSelfInConfig(&presentTwiceExternalState, newConfig, getServiceContext())
                          .getStatus());
    }

    {
        // Present once on the fast path, once on the slow path. This is expected to erroneously
        // succeed, because we should not check the slow path if we got a unique result on the fast
        // path.
        ReplicationCoordinatorExternalStateMock presentFastAndSlowExternalState;
        presentFastAndSlowExternalState.addSelf(HostAndPort("h2"));
        presentFastAndSlowExternalState.addSelfSlow(HostAndPort("h3"), Seconds(29));
        ASSERT_EQUALS(1,
                      unittest::assertGet(findSelfInConfig(
                          &presentFastAndSlowExternalState, newConfig, getServiceContext())));
    }

    {
        // Present only on the slow path, with a long timeout.  This will fail.
        ReplicationCoordinatorExternalStateMock presentLongTimeoutExternalState;
        presentLongTimeoutExternalState.addSelfSlow(HostAndPort("h2"), Seconds(31));
        ASSERT_EQUALS(
            ErrorCodes::NodeNotFound,
            findSelfInConfig(&presentLongTimeoutExternalState, newConfig, getServiceContext())
                .getStatus());
    }
}

TEST_F(ServiceContextTest, FindOwnHostInConfigQuick) {
    ReplSetConfig newConfig;
    newConfig = ReplSetConfig::parse(BSON("_id"
                                          << "rs0"
                                          << "version" << 2 << "protocolVersion" << 1 << "members"
                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                   << "h1:1234")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "h2:1234")
                                                        << BSON("_id" << 3 << "host"
                                                                      << "h3:1234")
                                                        << BSON("_id" << 4 << "host"
                                                                      << "h2:1234"))));

    // Does not exist.
    ASSERT_EQUALS(-1, findOwnHostInConfigQuick(newConfig, HostAndPort("non-existent")));

    // First in config, not duplicated.
    ASSERT_EQUALS(0, findOwnHostInConfigQuick(newConfig, HostAndPort("h1:1234")));

    // Not first in config but also not duplicated.
    ASSERT_EQUALS(2, findOwnHostInConfigQuick(newConfig, HostAndPort("h3:1234")));

    // First match in a tie.
    ASSERT_EQUALS(1, findOwnHostInConfigQuick(newConfig, HostAndPort("h2:1234")));
}

}  // namespace
}  // namespace repl
}  // namespace mongo
