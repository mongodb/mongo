// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/ddl/rename_collection_common.h"

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

#include <variant>

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
