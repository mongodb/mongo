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

#include "mongo/platform/basic.h"

#include "mongo/db/commands/write_commands/write_commands_common.h"

#include <algorithm>
#include <memory>
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

/**
 * Extracts the namespace being indexed from a raw BSON write command.
 * TODO: Remove when we have parsing hooked before authorization.
 */
NamespaceString _getIndexedNss(const std::vector<BSONObj>& documents) {
    uassert(ErrorCodes::FailedToParse, "index write batch is empty", !documents.empty());
    std::string ns = documents.front()["ns"].str();
    uassert(ErrorCodes::FailedToParse,
            "index write batch contains an invalid index descriptor",
            !ns.empty());
    uassert(ErrorCodes::FailedToParse,
            "index write batches may only contain a single index descriptor",
            documents.size() == 1);
    return NamespaceString(std::move(ns));
}

void fillPrivileges(const write_ops::Insert& op,
                    std::vector<Privilege>* privileges,
                    ActionSet* actions) {
    actions->addAction(ActionType::insert);
}

void fillPrivileges(const write_ops::Update& op,
                    std::vector<Privilege>* privileges,
                    ActionSet* actions) {
    actions->addAction(ActionType::update);
    // Upsert also requires insert privs
    const auto& updates = op.getUpdates();
    if (std::any_of(updates.begin(), updates.end(), [](auto&& x) { return x.getUpsert(); })) {
        actions->addAction(ActionType::insert);
    }
}

void fillPrivileges(const write_ops::Delete& op,
                    std::vector<Privilege>* privileges,
                    ActionSet* actions) {
    actions->addAction(ActionType::remove);
}

template <typename Op>
void checkAuthorizationImpl(AuthorizationSession* authzSession,
                            bool withDocumentValidationBypass,
                            const Op& op) {
    std::vector<Privilege> privileges;
    ActionSet actions;
    if (withDocumentValidationBypass) {
        actions.addAction(ActionType::bypassDocumentValidation);
    }
    fillPrivileges(op, &privileges, &actions);
    if (!actions.empty()) {
        privileges.push_back(
            Privilege(ResourcePattern::forExactNamespace(op.getNamespace()), actions));
    }
    uassert(ErrorCodes::Unauthorized,
            "unauthorized",
            authzSession->isAuthorizedForPrivileges(privileges));
}

}  // namespace

void checkAuthForInsertCommand(AuthorizationSession* authzSession,
                               bool withDocumentValidationBypass,
                               const write_ops::Insert& op) {
    checkAuthorizationImpl(authzSession, withDocumentValidationBypass, op);
}

void checkAuthForUpdateCommand(AuthorizationSession* authzSession,
                               bool withDocumentValidationBypass,
                               const write_ops::Update& op) {
    checkAuthorizationImpl(authzSession, withDocumentValidationBypass, op);
}

void checkAuthForDeleteCommand(AuthorizationSession* authzSession,
                               bool withDocumentValidationBypass,
                               const write_ops::Delete& op) {
    checkAuthorizationImpl(authzSession, withDocumentValidationBypass, op);
}

}  // namespace auth
}  // namespace mongo
