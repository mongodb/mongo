/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/auth/authorization_contract.h"

#include "mongo/base/string_data.h"
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

}  // namespace
}  // namespace mongo
