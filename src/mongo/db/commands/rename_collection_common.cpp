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

#include "mongo/db/commands/rename_collection_common.h"

#include <variant>

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"

namespace mongo {
namespace rename_collection {
namespace {

// Wrapper around AuthorizationSession::isAuthorizedForActionsOnResource which additionally requires
// the implicit privilege of any restricted namespace. Prefer this over calling AuthorizationSession
// directly from this file. See auth::getImplicitPrivilegeForRestrictedNamespace.
bool isAuthorizedForActionsOnNamespace(AuthorizationSession* authSession,
                                       const NamespaceString& ns,
                                       const ActionSet& actions) {
    const auto resource = ResourcePattern::forExactNamespace(ns);
    if (auto implicitPrivilege =
            auth::getImplicitPrivilegeForRestrictedNamespace(resource, actions)) {
        if (!authSession->isAuthorizedForPrivilege(*implicitPrivilege)) {
            return false;
        }
    }

    return authSession->isAuthorizedForActionsOnResource(resource, actions);
}

}  // namespace

Status checkAuthForRenameCollectionCommand(Client* client, const RenameCollectionCommand& request) {
    const auto& sourceNS = request.getCommandParameter();
    const auto& targetNS = request.getTo();
    const bool dropTarget = [&] {
        const auto dropTarget = request.getDropTarget();
        if (holds_alternative<bool>(dropTarget)) {
            return get<bool>(dropTarget);
        }

        // UUID alternative is "trueish"
        return true;
    }();

    auto* authSession = AuthorizationSession::get(client);

    // A rename within one database can be authorized by 'renameCollectionSameDB' on a single
    // database. However, a restricted namespace will need extra privileges so they must
    // undergo the more complete check below.
    if (!auth::isRestrictedNamespace(sourceNS) && !auth::isRestrictedNamespace(targetNS) &&
        sourceNS.dbName() == targetNS.dbName() && sourceNS.isNormalCollection() &&
        targetNS.isNormalCollection()) {
        const bool canRename = authSession->isAuthorizedForActionsOnResource(
            ResourcePattern::forDatabaseName(sourceNS.dbName()),
            ActionType::renameCollectionSameDB);

        bool canDropTargetIfNeeded = true;
        if (dropTarget) {
            canDropTargetIfNeeded = authSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(targetNS), ActionType::dropCollection);
        }

        bool canReadSrc = authSession->isAuthorizedForActionsOnResource(
            ResourcePattern::forExactNamespace(sourceNS), ActionType::find);
        bool canReadDest = authSession->isAuthorizedForActionsOnResource(
            ResourcePattern::forExactNamespace(targetNS), ActionType::find);

        // Even if the user can rename collections and can drop the target collection,
        // the user should not be able to rename a collection from one they can't read
        // to one they can.
        if (canRename && canDropTargetIfNeeded && (canReadSrc || !canReadDest)) {
            return Status::OK();
        }
    }

    // Check privileges on source collection
    ActionSet sourceActions{ActionType::find, ActionType::dropCollection};
    if (!isAuthorizedForActionsOnNamespace(authSession, sourceNS, sourceActions)) {
        return Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    // Check privileges on dest collection
    ActionSet targetActions{ActionType::insert, ActionType::createIndex};
    if (dropTarget) {
        targetActions.addAction(ActionType::dropCollection);
    }

    if (!isAuthorizedForActionsOnNamespace(authSession, targetNS, targetActions)) {
        return Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    return Status::OK();
}

}  // namespace rename_collection
}  // namespace mongo
