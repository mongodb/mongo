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

#include "mongo/platform/basic.h"

#include "mongo/db/commands/write_commands/write_commands_common.h"

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace auth {

using std::string;
using std::vector;

Status checkAuthForWriteCommand(AuthorizationSession* authzSession,
                                BatchedCommandRequest::BatchType cmdType,
                                const NamespaceString& cmdNSS,
                                const BSONObj& cmdObj) {
    vector<Privilege> privileges;
    ActionSet actionsOnCommandNSS;

    if (shouldBypassDocumentValidationForCommand(cmdObj)) {
        actionsOnCommandNSS.addAction(ActionType::bypassDocumentValidation);
    }

    if (cmdType == BatchedCommandRequest::BatchType_Insert) {
        if (!cmdNSS.isSystemDotIndexes()) {
            actionsOnCommandNSS.addAction(ActionType::insert);
        } else {
            // Special-case indexes until we have a command
            string nsToIndex, errMsg;
            if (!BatchedCommandRequest::getIndexedNS(cmdObj, &nsToIndex, &errMsg)) {
                return Status(ErrorCodes::FailedToParse, errMsg);
            }

            NamespaceString nssToIndex(nsToIndex);
            privileges.push_back(
                Privilege(ResourcePattern::forExactNamespace(nssToIndex), ActionType::createIndex));
        }
    } else if (cmdType == BatchedCommandRequest::BatchType_Update) {
        actionsOnCommandNSS.addAction(ActionType::update);

        // Upsert also requires insert privs
        if (BatchedCommandRequest::containsUpserts(cmdObj)) {
            actionsOnCommandNSS.addAction(ActionType::insert);
        }
    } else {
        fassert(17251, cmdType == BatchedCommandRequest::BatchType_Delete);
        actionsOnCommandNSS.addAction(ActionType::remove);
    }


    if (!actionsOnCommandNSS.empty()) {
        privileges.emplace_back(ResourcePattern::forExactNamespace(cmdNSS), actionsOnCommandNSS);
    }

    if (authzSession->isAuthorizedForPrivileges(privileges))
        return Status::OK();

    return Status(ErrorCodes::Unauthorized, "unauthorized");
}
}
}
