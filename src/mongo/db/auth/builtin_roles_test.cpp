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

/**
 * Unit tests of the builtin roles psuedo-collection.
 */

#include "mongo/db/auth/builtin_roles.h"

#include "mongo/base/string_data.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/database_name_util.h"

#include <vector>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>

namespace mongo {
namespace {

const auto kAdminDB = DatabaseName::kAdmin;
const auto kAdminRsrc = ResourcePattern::forDatabaseName(kAdminDB);
const auto kAdminSystemJSNSS =
    NamespaceString::createNamespaceString_forTest(kAdminDB, "system.js"_sd);
const auto kAdminSystemJSRsrc = ResourcePattern::forExactNamespace(kAdminSystemJSNSS);

TEST(BuiltinRoles, BuiltinRolesOnlyOnAppropriateDatabases) {
    ASSERT(auth::isBuiltinRole(RoleName("read", "test")));
    ASSERT(auth::isBuiltinRole(RoleName("readWrite", "test")));
    ASSERT(auth::isBuiltinRole(RoleName("userAdmin", "test")));
    ASSERT(auth::isBuiltinRole(RoleName("dbAdmin", "test")));
    ASSERT(auth::isBuiltinRole(RoleName("dbOwner", "test")));
    ASSERT(auth::isBuiltinRole(RoleName("enableSharding", "test")));
    ASSERT(!auth::isBuiltinRole(RoleName("readAnyDatabase", "test")));
    ASSERT(!auth::isBuiltinRole(RoleName("readWriteAnyDatabase", "test")));
    ASSERT(!auth::isBuiltinRole(RoleName("userAdminAnyDatabase", "test")));
    ASSERT(!auth::isBuiltinRole(RoleName("dbAdminAnyDatabase", "test")));
    ASSERT(!auth::isBuiltinRole(RoleName("clusterAdmin", "test")));
    ASSERT(!auth::isBuiltinRole(RoleName("root", "test")));
    ASSERT(!auth::isBuiltinRole(RoleName("__system", "test")));
    ASSERT(!auth::isBuiltinRole(RoleName("MyRole", "test")));
    ASSERT(!auth::isBuiltinRole(RoleName("searchCoordinator", "test")));

    ASSERT(auth::isBuiltinRole(RoleName("read", "admin")));
    ASSERT(auth::isBuiltinRole(RoleName("readWrite", "admin")));
    ASSERT(auth::isBuiltinRole(RoleName("userAdmin", "admin")));
    ASSERT(auth::isBuiltinRole(RoleName("dbAdmin", "admin")));
    ASSERT(auth::isBuiltinRole(RoleName("dbOwner", "admin")));
    ASSERT(auth::isBuiltinRole(RoleName("enableSharding", "admin")));
    ASSERT(auth::isBuiltinRole(RoleName("readAnyDatabase", "admin")));
    ASSERT(auth::isBuiltinRole(RoleName("readWriteAnyDatabase", "admin")));
    ASSERT(auth::isBuiltinRole(RoleName("userAdminAnyDatabase", "admin")));
    ASSERT(auth::isBuiltinRole(RoleName("dbAdminAnyDatabase", "admin")));
    ASSERT(auth::isBuiltinRole(RoleName("clusterAdmin", "admin")));
    ASSERT(auth::isBuiltinRole(RoleName("root", "admin")));
    ASSERT(auth::isBuiltinRole(RoleName("__system", "admin")));
    ASSERT(auth::isBuiltinRole(RoleName("directShardOperations", "admin")));
    ASSERT(!auth::isBuiltinRole(RoleName("MyRole", "admin")));
    ASSERT(auth::isBuiltinRole(RoleName("searchCoordinator", "admin")));
}

TEST(BuiltinRoles, getBuiltinRolesForDB) {
    auto adminRoles = auth::getBuiltinRoleNamesForDB(DatabaseName::kAdmin);
    ASSERT(adminRoles.contains(RoleName("read", "admin")));
    ASSERT(adminRoles.contains(RoleName("readAnyDatabase", "admin")));
    for (const auto& role : adminRoles) {
        ASSERT_EQ(role.getDB(), "admin");
        ASSERT(auth::isBuiltinRole(role));
    }

    auto testRoles = auth::getBuiltinRoleNamesForDB(
        DatabaseName::createDatabaseName_forTest(boost::none, "test"));
    ASSERT(testRoles.contains(RoleName("read", "test")));
    ASSERT(!testRoles.contains(RoleName("readAnyDatabase", "test")));
    for (const auto& role : testRoles) {
        ASSERT_EQ(role.getDB(), "test");
        ASSERT(auth::isBuiltinRole(role));
    }
    ASSERT_GTE(adminRoles.size(), testRoles.size());
}

TEST(BuiltinRoles, addPrivilegesForBuiltinRole) {
    PrivilegeVector privs;
    ASSERT(auth::addPrivilegesForBuiltinRole(RoleName("read", "admin"), &privs));
    ASSERT_EQ(privs.size(), 2);

    // The "read" role should have these actions on the admin DB and admin.system.js collections.
    const ActionSet expSet({
        ActionType::changeStream,
        ActionType::collStats,
        ActionType::dbHash,
        ActionType::dbStats,
        ActionType::find,
        ActionType::killCursors,
        ActionType::listCollections,
        ActionType::listIndexes,
        ActionType::listSearchIndexes,
        ActionType::performRawDataOperations,
        ActionType::planCacheRead,
    });

    for (const auto& priv : privs) {
        auto resource = priv.getResourcePattern();
        ASSERT((resource == kAdminRsrc) || (resource == kAdminSystemJSRsrc));
        ASSERT(priv.getActions() == expSet);
    }
}

TEST(BuiltinRoles, addSystemBucketsPrivilegesForBuiltinRoleClusterManager) {
    PrivilegeVector privs;
    ASSERT(auth::addPrivilegesForBuiltinRole(RoleName("clusterManager", "admin"), &privs));
    ASSERT_EQ(privs.size(), 11);

    const auto systemBucketsResourcePattern = ResourcePattern::forAnySystemBuckets(boost::none);

    const ActionSet clusterManagerRoleDatabaseActionSet({
        ActionType::clearJumboFlag,
        ActionType::splitChunk,
        ActionType::moveChunk,
        ActionType::moveCollection,
        ActionType::enableSharding,
        ActionType::splitVector,
        ActionType::refineCollectionShardKey,
        ActionType::reshardCollection,
        ActionType::analyzeShardKey,
        ActionType::configureQueryAnalyzer,
        ActionType::unshardCollection,
    });

    for (const auto& priv : privs) {
        auto resourcePattern = priv.getResourcePattern();
        if (resourcePattern == systemBucketsResourcePattern) {
            ASSERT(priv.getActions() == clusterManagerRoleDatabaseActionSet);
        }
    }
}
}  // namespace
}  // namespace mongo
