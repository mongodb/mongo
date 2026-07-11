// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/ddl/rename_collection_common.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"

#include <variant>

namespace mongo {
namespace rename_collection {

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

    if (sourceNS.dbName() == targetNS.dbName() && sourceNS.isNormalCollection() &&
        targetNS.isNormalCollection()) {
        const bool canRename = AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
            ResourcePattern::forDatabaseName(sourceNS.dbName()),
            ActionType::renameCollectionSameDB);

        bool canDropTargetIfNeeded = true;
        if (dropTarget) {
            canDropTargetIfNeeded =
                AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                    ResourcePattern::forExactNamespace(targetNS), ActionType::dropCollection);
        }

        bool canReadSrc = AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
            ResourcePattern::forExactNamespace(sourceNS), ActionType::find);
        bool canReadDest = AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
            ResourcePattern::forExactNamespace(targetNS), ActionType::find);

        // Even if the user can rename collections and can drop the target collection,
        // the user should not be able to rename a collection from one they can't read
        // to one they can.
        if (canRename && canDropTargetIfNeeded && (canReadSrc || !canReadDest)) {
            return Status::OK();
        }
    }

    // Check privileges on source collection
    ActionSet actions;
    actions.addAction(ActionType::find);
    actions.addAction(ActionType::dropCollection);
    if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
            ResourcePattern::forExactNamespace(sourceNS), actions)) {
        return Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    // Check privileges on dest collection
    actions.removeAllActions();
    actions.addAction(ActionType::insert);
    actions.addAction(ActionType::createIndex);
    if (dropTarget) {
        actions.addAction(ActionType::dropCollection);
    }
    if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
            ResourcePattern::forExactNamespace(targetNS), actions)) {
        return Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    return Status::OK();
}

}  // namespace rename_collection
}  // namespace mongo
