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
#include "mongo/db/ops/write_ops.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace auth {
namespace {

using write_ops::Insert;
using write_ops::Update;
using write_ops::UpdateOpEntry;

/**
 * Helper to determine whether or not there are any upserts in the batch
 */
bool containsUpserts(const BSONObj& writeCmdObj) {
    BSONElement updatesEl = writeCmdObj[Update::kUpdatesFieldName];
    if (updatesEl.type() != Array) {
        return false;
    }

    for (const auto& updateEl : updatesEl.Array()) {
        if (!updateEl.isABSONObj())
            continue;

        if (updateEl.Obj()[UpdateOpEntry::kUpsertFieldName].trueValue())
            return true;
    }

    return false;
}

/**
 * Helper to extract the namespace being indexed from a raw BSON write command.
 *
 * TODO: Remove when we have parsing hooked before authorization.
 */
StatusWith<NamespaceString> getIndexedNss(const BSONObj& writeCmdObj) {
    BSONElement documentsEl = writeCmdObj[Insert::kDocumentsFieldName];
    if (documentsEl.type() != Array) {
        return {ErrorCodes::FailedToParse, "index write batch is invalid"};
    }

    BSONObjIterator it(documentsEl.Obj());
    if (!it.more()) {
        return {ErrorCodes::FailedToParse, "index write batch is empty"};
    }

    BSONElement indexDescEl = it.next();

    const std::string nsToIndex = indexDescEl["ns"].str();
    if (nsToIndex.empty()) {
        return {ErrorCodes::FailedToParse,
                "index write batch contains an invalid index descriptor"};
    }

    if (it.more()) {
        return {ErrorCodes::FailedToParse,
                "index write batches may only contain a single index descriptor"};
    }

    return {NamespaceString(std::move(nsToIndex))};
}

}  // namespace

Status checkAuthForWriteCommand(AuthorizationSession* authzSession,
                                BatchedCommandRequest::BatchType cmdType,
                                const NamespaceString& cmdNSS,
                                const BSONObj& cmdObj) {
    std::vector<Privilege> privileges;
    ActionSet actionsOnCommandNSS;

    if (shouldBypassDocumentValidationForCommand(cmdObj)) {
        actionsOnCommandNSS.addAction(ActionType::bypassDocumentValidation);
    }

    if (cmdType == BatchedCommandRequest::BatchType_Insert) {
        if (!cmdNSS.isSystemDotIndexes()) {
            actionsOnCommandNSS.addAction(ActionType::insert);
        } else {
            // Special-case indexes until we have a command
            const auto swNssToIndex = getIndexedNss(cmdObj);
            if (!swNssToIndex.isOK()) {
                return swNssToIndex.getStatus();
            }

            const auto& nssToIndex = swNssToIndex.getValue();
            privileges.push_back(
                Privilege(ResourcePattern::forExactNamespace(nssToIndex), ActionType::createIndex));
        }
    } else if (cmdType == BatchedCommandRequest::BatchType_Update) {
        actionsOnCommandNSS.addAction(ActionType::update);

        // Upsert also requires insert privs
        if (containsUpserts(cmdObj)) {
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

}  // namespace auth
}  // namespace mongo
