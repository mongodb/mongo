// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_contract.h"

#include "mongo/db/auth/access_checks_gen.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/unittest.h"

#include <initializer_list>
#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
namespace {

TEST(AuthContractTest, Basic) {

    AuthorizationContract ac;
    ac.addAccessCheck(AccessCheckEnum::kIsAuthenticated);
    ac.addAccessCheck(AccessCheckEnum::kIsCoAuthorized);

    ActionSet enableShardingActions;
    enableShardingActions.addAction(ActionType::enableSharding);
    enableShardingActions.addAction(ActionType::refineCollectionShardKey);
    enableShardingActions.addAction(ActionType::reshardCollection);
    ac.addPrivilege(
        Privilege(ResourcePattern::forAnyNormalResource(boost::none), enableShardingActions));

    ASSERT_TRUE(ac.hasAccessCheck(AccessCheckEnum::kIsAuthenticated));
    ASSERT_TRUE(ac.hasAccessCheck(AccessCheckEnum::kIsCoAuthorized));

    ASSERT_FALSE(ac.hasAccessCheck(AccessCheckEnum::kIsAuthorizedToParseNamespaceElement));


    ASSERT_TRUE(ac.hasPrivileges(
        Privilege(ResourcePattern::forAnyNormalResource(boost::none), ActionType::enableSharding)));
    ASSERT_TRUE(ac.hasPrivileges(Privilege(ResourcePattern::forAnyNormalResource(boost::none),
                                           ActionType::refineCollectionShardKey)));
    ASSERT_TRUE(ac.hasPrivileges(Privilege(ResourcePattern::forAnyNormalResource(boost::none),
                                           ActionType::reshardCollection)));

    ASSERT_FALSE(ac.hasPrivileges(
        Privilege(ResourcePattern::forAnyNormalResource(boost::none), ActionType::shutdown)));
    ASSERT_FALSE(ac.hasPrivileges(
        Privilege(ResourcePattern::forClusterResource(boost::none), ActionType::enableSharding)));


    ASSERT_TRUE(ac.hasPrivileges(
        Privilege(ResourcePattern::forAnyNormalResource(boost::none), enableShardingActions)));

    ASSERT_TRUE(ac.contains(ac));
}

TEST(AuthContractTest, SimpleAccessCheck) {

    AuthorizationContract ac;
    ac.addAccessCheck(AccessCheckEnum::kIsAuthenticated);
    ac.addAccessCheck(AccessCheckEnum::kIsCoAuthorized);

    AuthorizationContract empty;

    ASSERT_TRUE(ac.contains(empty));
    ASSERT_FALSE(empty.contains(ac));
}

TEST(AuthContractTest, DifferentAccessCheck) {

    AuthorizationContract ac1;
    ac1.addAccessCheck(AccessCheckEnum::kIsAuthenticated);
    ac1.addAccessCheck(AccessCheckEnum::kIsCoAuthorized);

    AuthorizationContract ac2;
    ac2.addAccessCheck(AccessCheckEnum::kIsAuthenticated);
    ac2.addAccessCheck(AccessCheckEnum::kIsAuthorizedToParseNamespaceElement);

    ASSERT_FALSE(ac1.contains(ac2));
    ASSERT_FALSE(ac2.contains(ac1));
}

TEST(AuthContractTest, SimplePrivilege) {

    AuthorizationContract ac;
    ac.addPrivilege(
        Privilege(ResourcePattern::forAnyNormalResource(boost::none), ActionType::enableSharding));

    AuthorizationContract empty;

    ASSERT_TRUE(ac.contains(empty));
    ASSERT_FALSE(empty.contains(ac));
}


TEST(AuthContractTest, DifferentResoucePattern) {

    AuthorizationContract ac1;
    ac1.addPrivilege(
        Privilege(ResourcePattern::forAnyNormalResource(boost::none), ActionType::enableSharding));

    AuthorizationContract ac2;
    ac2.addPrivilege(
        Privilege(ResourcePattern::forClusterResource(boost::none), ActionType::enableSharding));

    ASSERT_FALSE(ac1.contains(ac2));
    ASSERT_FALSE(ac2.contains(ac1));
}


TEST(AuthContractTest, DifferentActionType) {

    AuthorizationContract ac1;
    ac1.addPrivilege(
        Privilege(ResourcePattern::forAnyNormalResource(boost::none), ActionType::enableSharding));

    AuthorizationContract ac2;
    ac2.addPrivilege(Privilege(ResourcePattern::forAnyNormalResource(boost::none),
                               ActionType::grantPrivilegesToRole));

    ASSERT_FALSE(ac1.contains(ac2));
    ASSERT_FALSE(ac2.contains(ac1));
}

TEST(AuthContractTest, InitializerList) {

    AuthorizationContract ac1;
    ac1.addAccessCheck(AccessCheckEnum::kIsAuthenticated);
    ac1.addAccessCheck(AccessCheckEnum::kIsCoAuthorized);

    ActionSet enableShardingActions;
    enableShardingActions.addAction(ActionType::enableSharding);
    enableShardingActions.addAction(ActionType::refineCollectionShardKey);
    enableShardingActions.addAction(ActionType::reshardCollection);
    ac1.addPrivilege(
        Privilege(ResourcePattern::forAnyNormalResource(boost::none), enableShardingActions));

    AuthorizationContract ac2(
        std::initializer_list<AccessCheckEnum>{AccessCheckEnum::kIsAuthenticated,
                                               AccessCheckEnum::kIsCoAuthorized},
        std::initializer_list<Privilege>{
            Privilege(ResourcePattern::forAnyNormalResource(boost::none),
                      {ActionType::enableSharding,
                       ActionType::refineCollectionShardKey,
                       ActionType::reshardCollection})});

    ASSERT_TRUE(ac1.contains(ac2));
    ASSERT_TRUE(ac2.contains(ac1));
}

TEST(AuthContractTest, NonTestModeCheck) {
    AuthorizationContract ac(/* isTestModeEnabled */ false);
    ac.addAccessCheck(AccessCheckEnum::kIsAuthenticated);
    ac.addAccessCheck(AccessCheckEnum::kIsCoAuthorized);

    ActionSet enableShardingActions;
    enableShardingActions.addAction(ActionType::enableSharding);
    enableShardingActions.addAction(ActionType::refineCollectionShardKey);
    enableShardingActions.addAction(ActionType::reshardCollection);
    ac.addPrivilege(
        Privilege(ResourcePattern::forAnyNormalResource(boost::none), enableShardingActions));

    // Non-test mode will not keep accounting and will not take any mutex guard
    ASSERT_FALSE(ac.hasAccessCheck(AccessCheckEnum::kIsAuthenticated));
    ASSERT_FALSE(ac.hasAccessCheck(AccessCheckEnum::kIsCoAuthorized));

    ASSERT_FALSE(ac.hasPrivileges(
        Privilege(ResourcePattern::forAnyNormalResource(boost::none), ActionType::enableSharding)));
    ASSERT_FALSE(ac.hasPrivileges(Privilege(ResourcePattern::forAnyNormalResource(boost::none),
                                            ActionType::refineCollectionShardKey)));
    ASSERT_FALSE(ac.hasPrivileges(Privilege(ResourcePattern::forAnyNormalResource(boost::none),
                                            ActionType::reshardCollection)));
}

TEST(AuthContractTest, CommandDepthTracking) {
    AuthorizationContract ac;

    // Start tracking for top-level command
    ac.enterCommandScope();

    // Add checks at depth 1 (should be recorded)
    ac.addAccessCheck(AccessCheckEnum::kIsAuthenticated);
    ac.addPrivilege(
        Privilege(ResourcePattern::forAnyNormalResource(boost::none), ActionType::find));

    ASSERT_TRUE(ac.hasAccessCheck(AccessCheckEnum::kIsAuthenticated));
    ASSERT_TRUE(ac.hasPrivileges(
        Privilege(ResourcePattern::forAnyNormalResource(boost::none), ActionType::find)));

    // Start nested command (depth 2)
    ac.enterCommandScope();

    // Add checks at depth 2 (should be ignored)
    ac.addAccessCheck(AccessCheckEnum::kIsCoAuthorized);
    ac.addPrivilege(
        Privilege(ResourcePattern::forClusterResource(boost::none), ActionType::insert));

    // Original checks should still be there, new ones should not
    ASSERT_TRUE(ac.hasAccessCheck(AccessCheckEnum::kIsAuthenticated));
    ASSERT_TRUE(ac.hasPrivileges(
        Privilege(ResourcePattern::forAnyNormalResource(boost::none), ActionType::find)));
    ASSERT_FALSE(ac.hasAccessCheck(AccessCheckEnum::kIsCoAuthorized));
    ASSERT_FALSE(ac.hasPrivileges(
        Privilege(ResourcePattern::forClusterResource(boost::none), ActionType::insert)));

    // End nested command
    ac.exitCommandScope();

    // Back at depth 1, can add checks again
    ac.addAccessCheck(AccessCheckEnum::kIsAuthorizedToParseNamespaceElement);
    ASSERT_TRUE(ac.hasAccessCheck(AccessCheckEnum::kIsAuthorizedToParseNamespaceElement));

    // End top-level command
    ac.exitCommandScope();
}

TEST(AuthContractTest, ClearOnlyAtTopLevel) {
    AuthorizationContract ac;

    // Start first top-level command
    ac.enterCommandScope();
    ac.addAccessCheck(AccessCheckEnum::kIsAuthenticated);
    ASSERT_TRUE(ac.hasAccessCheck(AccessCheckEnum::kIsAuthenticated));
    ac.exitCommandScope();

    // Start second top-level command - should clear previous checks
    ac.enterCommandScope();
    ASSERT_FALSE(ac.hasAccessCheck(AccessCheckEnum::kIsAuthenticated));

    ac.addAccessCheck(AccessCheckEnum::kIsCoAuthorized);
    ASSERT_TRUE(ac.hasAccessCheck(AccessCheckEnum::kIsCoAuthorized));

    // Start nested command - should NOT clear
    ac.enterCommandScope();
    ASSERT_TRUE(ac.hasAccessCheck(AccessCheckEnum::kIsCoAuthorized));

    ac.exitCommandScope();
    ac.exitCommandScope();
}

}  // namespace
}  // namespace mongo
