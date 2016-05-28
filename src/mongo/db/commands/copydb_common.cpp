/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/commands/copydb.h"

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"

namespace mongo {
namespace copydb {

Status checkAuthForCopydbCommand(ClientBasic* client,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) {
    bool fromSelf = StringData(cmdObj.getStringField("fromhost")).empty();
    StringData fromdb = cmdObj.getStringField("fromdb");
    StringData todb = cmdObj.getStringField("todb");

    // get system collections
    std::vector<std::string> legalClientSystemCollections;
    legalClientSystemCollections.push_back("system.js");
    if (fromdb == "admin") {
        legalClientSystemCollections.push_back("system.users");
        legalClientSystemCollections.push_back("system.roles");
        legalClientSystemCollections.push_back("system.version");
    } else if (fromdb == "local") {  // TODO(spencer): shouldn't be possible. See SERVER-11383
        legalClientSystemCollections.push_back("system.replset");
    }

    // Check authorization on destination db
    ActionSet actions;
    actions.addAction(ActionType::insert);
    actions.addAction(ActionType::createIndex);
    if (shouldBypassDocumentValidationForCommand(cmdObj)) {
        actions.addAction(ActionType::bypassDocumentValidation);
    }

    if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
            ResourcePattern::forDatabaseName(todb), actions)) {
        return Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    actions.removeAllActions();
    actions.addAction(ActionType::insert);
    for (size_t i = 0; i < legalClientSystemCollections.size(); ++i) {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnNamespace(
                NamespaceString(todb, legalClientSystemCollections[i]), actions)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
    }

    if (fromSelf) {
        // If copying from self, also require privileges on source db
        actions.removeAllActions();
        actions.addAction(ActionType::find);
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(fromdb), actions)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        for (size_t i = 0; i < legalClientSystemCollections.size(); ++i) {
            if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnNamespace(
                    NamespaceString(fromdb, legalClientSystemCollections[i]), actions)) {
                return Status(ErrorCodes::Unauthorized, "Unauthorized");
            }
        }
    }
    return Status::OK();
}

}  // namespace copydb
}  // namespace mongo
