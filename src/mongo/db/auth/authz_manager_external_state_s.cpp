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

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/shim.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/authorization_client_handle.h"
#include "mongo/db/auth/authz_manager_external_state_s.h"
#include "mongo/db/auth/authz_session_external_state.h"
#include "mongo/db/auth/authz_session_external_state_router.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/commands/user_management_commands_gen.h"
#include "mongo/db/multitenancy.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {

/**
 * Returns the top level field which is expected to be returned by rolesInfo.
 */
std::string rolesFieldName(PrivilegeFormat showPrivileges) {
    if (showPrivileges == PrivilegeFormat::kShowAsUserFragment) {
        return "userFragment";
    }
    return "roles";
}

/**
 * Attches a string representation of a PrivilegeFormat to the provided BSONObjBuilder.
 */
void addShowToBuilder(BSONObjBuilder* builder,
                      PrivilegeFormat showPrivileges,
                      AuthenticationRestrictionsFormat showRestrictions) {
    if (showPrivileges == PrivilegeFormat::kShowAsUserFragment) {
        builder->append("showPrivileges", "asUserfragment");
    } else {
        builder->append("showPrivileges", showPrivileges == PrivilegeFormat::kShowSeparate);
        builder->append("showAuthenticationRestrictions",
                        showRestrictions == AuthenticationRestrictionsFormat::kShow);
    }
}

}  // namespace

AuthzManagerExternalStateMongos::AuthzManagerExternalStateMongos() = default;

AuthzManagerExternalStateMongos::~AuthzManagerExternalStateMongos() = default;

std::unique_ptr<AuthzSessionExternalState>
AuthzManagerExternalStateMongos::makeAuthzSessionExternalState(Client* client) {
    return std::make_unique<AuthzSessionExternalStateRouter>(client);
}

StatusWith<User> AuthzManagerExternalStateMongos::getUserObject(
    OperationContext* opCtx,
    const UserRequest& userReq,
    const SharedUserAcquisitionStats& userAcquisitionStats) {
    // Marshalling to BSON and back is inevitable since the
    // source of truth is a system external to mongos.
    BSONObj userDoc;
    auto status = getUserDescription(opCtx, userReq, &userDoc, userAcquisitionStats);
    if (!status.isOK()) {
        return status;
    }

    User user(userReq.clone());
    V2UserDocumentParser dp;
    dp.setTenantId(getActiveTenant(opCtx));
    status = dp.initializeUserFromUserDocument(userDoc, &user);
    if (!status.isOK()) {
        return status;
    }

    return std::move(user);
}

Status AuthzManagerExternalStateMongos::getUserDescription(
    OperationContext* opCtx,
    const UserRequest& userReq,
    BSONObj* result,
    const SharedUserAcquisitionStats& userAcquisitionStats) {
    return AuthorizationManager::get(opCtx->getService())
        ->lookupUserDescription(opCtx, userReq.getUserName(), result);
}

Status AuthzManagerExternalStateMongos::rolesExist(OperationContext* opCtx,
                                                   const std::vector<RoleName>& roleNames) try {
    return AuthorizationManager::get(opCtx->getService())->rolesExist(opCtx, roleNames);
} catch (const DBException& ex) {
    return ex.toStatus();
}

bool AuthzManagerExternalStateMongos::hasAnyPrivilegeDocuments(OperationContext* opCtx) {
    return AuthorizationManager::get(opCtx->getService())->hasAnyPrivilegeDocuments(opCtx);
}

namespace {

std::unique_ptr<AuthzManagerExternalState> authzManagerExternalStateCreateImpl() {
    return std::make_unique<AuthzManagerExternalStateMongos>();
}

auto authzManagerExternalStateCreateRegistration = MONGO_WEAK_FUNCTION_REGISTRATION(
    AuthzManagerExternalState::create, authzManagerExternalStateCreateImpl);

}  // namespace

}  // namespace mongo
