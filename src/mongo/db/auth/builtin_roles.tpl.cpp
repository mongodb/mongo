/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include "mongo/db/auth/builtin_roles.h"

#include <map>

#include "mongo/base/string_data.h"
#include "mongo/base/string_data_comparator.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/multitenancy_gen.h"

namespace mongo {
namespace {
constexpr auto kAdminDB = "admin"_sd;

/* Forward declarations so that inheritance calls don't have to worry about order.
 * Warning: Potential for infinite recursion here, so inherit your roles wisely.
 */

//#for $role in $roles
void addPrivileges_${role.name}(PrivilegeVector* privileges, const DatabaseName& dbName);
//#end for

/* Implemenations */

//## Use current name if `db` wasn't specified,
//## or override it with an explicit value if it was.
//#def dbName($db)
//#if $db is None
//#echo 'dbName'
//#else
//#echo 'DatabaseNameUtil::deserialize(dbName.tenantId(), "' + $db + '"_sd,'
//#echo 'SerializationContext::stateDefault())'
//#end if
//#end def

//## Create a predicate for a privilege.
//#def privCondition($priv)
//#if $priv.tenancy == 'single'
//#echo 'if (!gMultitenancySupport)'
//#elif $priv.tenancy == 'multi'
//#echo 'if (gMultitenancySupport)'
//#elif $priv.tenancy == 'system'
//#echo 'if (dbName.tenantId() == boost::none)'
//#elif $priv.tenancy == 'tenant'
//#echo 'if (dbName.tenantId() != boost::none)'
//#else
//#assert $priv.tenancy == 'any'
//#end if
//#end def


//#for $role in $roles
void addPrivileges_${role.name}(PrivilegeVector* privileges, const DatabaseName& dbName) {
    //#if $role.adminOnly
    /* Admin only builtin role */
    fassert(6837401, dbName.isAdminDB());
    //#end if

    //#for $subrole in $role.roles
    addPrivileges_${subrole.role}(privileges, $dbName($subrole.db));
    //#end for

    //#for $priv in $role.privileges
    $privCondition($priv) {
        Privilege::addPrivilegeToPrivilegeVector(
            privileges,
            Privilege(
                //#if $priv.matchType == 'any'
                ResourcePattern::forAnyResource(dbName.tenantId()),
                //#elif $priv.matchType == 'any_normal'
                ResourcePattern::forAnyNormalResource(dbName.tenantId()),
                //#elif $priv.matchType == 'cluster'
                ResourcePattern::forClusterResource(dbName.tenantId()),
                //#elif $priv.matchType == 'database'
                ResourcePattern::forDatabaseName($dbName($priv.db)),
                //#elif $priv.matchType == 'collection'
                ResourcePattern::forCollectionName(dbName.tenantId(), "$priv.collection"_sd),
                //#elif $priv.matchType == 'exact_namespace'
                ResourcePattern::forExactNamespace(
                    NamespaceStringUtil::deserialize($dbName($priv.db), "$priv.collection"_sd)),
                //#elif $priv.matchType == 'any_system_buckets'
                ResourcePattern::forAnySystemBuckets(dbName.tenantId()),
                //#elif $priv.matchType == 'system_buckets_in_any_db'
                ResourcePattern::forAnySystemBucketsInAnyDatabase(dbName.tenantId(),
                                                                  "$priv.system_buckets"_sd),
                //#elif $priv.matchType == 'system_buckets'
                ResourcePattern::forExactSystemBucketsCollection($dbName($priv.db),
                                                                 "$priv.system_buckets"_sd),
                //#elif $priv.matchType == 'any_system_buckets_in_db'
                ResourcePattern::forAnySystemBucketsInDatabase($dbName($priv.db)),
                //#else
                //#assert False
                //#end if
                ActionSet({
                    //#for action in $priv.actions
                    ActionType::$action,
                    //#end for
                })));
    }
    //#end for

    //## Special case for __system.admin
    //#if $role.name == "__system"
    //#assert $role.adminOnly
    ActionSet allActions;
    allActions.addAllActions();
    Privilege::addPrivilegeToPrivilegeVector(
        privileges,
        Privilege(ResourcePattern::forAnyResource(dbName.tenantId()), std::move(allActions)));
    //#end if
}

//#end for

using addPrivilegesFn = void (*)(PrivilegeVector*, const DatabaseName&);
struct BuiltinRoleAttributes {
    bool adminOnly;
    addPrivilegesFn addPrivileges;
};

//#def boolval($val)
//#if $val
//#echo 'true'
//#else
//#echo 'false'
//#end if
//#end def

const std::map<StringData, BuiltinRoleAttributes> kBuiltinRoleMap = {
    //#for $role in $roles
    {"$role.name"_sd, {$boolval($role.adminOnly), addPrivileges_${role.name}}},
    //#end for
};

const stdx::unordered_set<RoleName> kAdminBuiltinRolesNoTenant = {
    //#for $role in $roles
    RoleName("$role.name"_sd, DatabaseName::kAdmin.db(omitTenant)),
    //#end for
};

// \$external is a virtual database used for X509, LDAP,
// and other authentication mechanisms and not used for storage.
// Therefore, granting privileges on this database does not make sense.
bool isValidDB(const DatabaseName& dbname) {
    return DatabaseName::isValid(dbname, DatabaseName::DollarInDbNameBehavior::Allow) &&
        (!dbname.isExternalDB());
}

}  // namespace

//#set $global_roles = [role for role in $roles if role.adminOnly == False]
//#set $admin_only_roles = [role for role in $roles if role.adminOnly == True]
//##
stdx::unordered_set<RoleName> auth::getBuiltinRoleNamesForDB(const DatabaseName& dbName) {
    if (!isValidDB(dbName)) {
        return {};
    }

    if (dbName.isAdminDB()) {
        if (dbName.tenantId() == boost::none) {
            // Specialcase for the admin DB in non-multitenancy mode.
            return kAdminBuiltinRolesNoTenant;
        }
        return stdx::unordered_set<RoleName>({
            //#for $role in $roles
            RoleName("$role.name"_sd, dbName),
            //#end for
        });

    } else {
        return stdx::unordered_set<RoleName>({
            //#for $role in $global_roles
            RoleName("$role.name"_sd, dbName),
            //#end for
        });
    }
}

void auth::generateUniversalPrivileges(PrivilegeVector* privileges,
                                       const boost::optional<TenantId>& tenantId) {
    addPrivileges___system(
        privileges,
        DatabaseNameUtil::deserialize(tenantId, kAdminDB, SerializationContext::stateDefault()));
}

bool auth::addPrivilegesForBuiltinRole(const RoleName& role, PrivilegeVector* privileges) {
    if (!isValidDB(role.getDatabaseName())) {
        return false;
    }

    auto it = kBuiltinRoleMap.find(role.getRole());
    if (it == kBuiltinRoleMap.end()) {
        return false;
    }

    const auto& def = it->second;
    if (def.adminOnly && (role.getDB() != kAdminDB)) {
        return false;
    }

    def.addPrivileges(privileges, role.getDatabaseName());
    return true;
}

bool auth::isBuiltinRole(const RoleName& role) {
    if (!isValidDB(role.getDatabaseName())) {
        return false;
    }

    auto it = kBuiltinRoleMap.find(role.getRole());
    if (it == kBuiltinRoleMap.end()) {
        return false;
    }

    return !it->second.adminOnly || (role.getDB() == kAdminDB);
}

}  // namespace mongo
