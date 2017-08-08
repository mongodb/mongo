/**
*    Copyright (C) 2012 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/db/commands/rename_collection.h"

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"

namespace mongo {
namespace rename_collection {

Status checkAuthForRenameCollectionCommand(Client* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
    const auto sourceNsElt = cmdObj["renameCollection"];
    const auto targetNsElt = cmdObj["to"];

    uassert(ErrorCodes::TypeMismatch,
            "'renameCollection' must be of type String",
            sourceNsElt.type() == BSONType::String);
    uassert(ErrorCodes::TypeMismatch,
            "'to' must be of type String",
            targetNsElt.type() == BSONType::String);

    const NamespaceString sourceNS(sourceNsElt.valueStringData());
    const NamespaceString targetNS(targetNsElt.valueStringData());
    bool dropTarget = cmdObj["dropTarget"].trueValue();

    if (sourceNS.db() == targetNS.db() && !sourceNS.isSystem() && !targetNS.isSystem()) {
        bool canRename = AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
            ResourcePattern::forDatabaseName(sourceNS.db()), ActionType::renameCollectionSameDB);

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
