/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MERIZO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kAccessControl

#include "merizo/platform/basic.h"

#include "merizo/db/auth/authz_manager_external_state_d.h"

#include "merizo/base/status.h"
#include "merizo/db/auth/authz_session_external_state_d.h"
#include "merizo/db/auth/user_name.h"
#include "merizo/db/client.h"
#include "merizo/db/db_raii.h"
#include "merizo/db/dbdirectclient.h"
#include "merizo/db/dbhelpers.h"
#include "merizo/db/jsobj.h"
#include "merizo/db/operation_context.h"
#include "merizo/db/service_context.h"
#include "merizo/db/storage/storage_engine.h"
#include "merizo/stdx/memory.h"
#include "merizo/util/assert_util.h"
#include "merizo/util/log.h"
#include "merizo/util/merizoutils/str.h"

namespace merizo {

AuthzManagerExternalStateMerizod::AuthzManagerExternalStateMerizod() = default;
AuthzManagerExternalStateMerizod::~AuthzManagerExternalStateMerizod() = default;

std::unique_ptr<AuthzSessionExternalState>
AuthzManagerExternalStateMerizod::makeAuthzSessionExternalState(AuthorizationManager* authzManager) {
    return stdx::make_unique<AuthzSessionExternalStateMerizod>(authzManager);
}

class AuthzLock : public AuthzManagerExternalState::StateLock {
public:
    explicit AuthzLock(OperationContext* opCtx)
        : _lock(opCtx,
                AuthorizationManager::usersCollectionNamespace.db(),
                MODE_S,
                opCtx->getDeadline()) {}

    static bool isLocked(OperationContext* opCtx);

private:
    Lock::DBLock _lock;
};

bool AuthzLock::isLocked(OperationContext* opCtx) {
    return opCtx->lockState()->isDbLockedForMode(
        AuthorizationManager::usersCollectionNamespace.db(), MODE_S);
}

std::unique_ptr<AuthzManagerExternalState::StateLock> AuthzManagerExternalStateMerizod::lock(
    OperationContext* opCtx) {
    return std::make_unique<AuthzLock>(opCtx);
}

bool AuthzManagerExternalStateMerizod::needsLockForUserName(OperationContext* opCtx,
                                                           const UserName& name) {
    return (shouldUseRolesFromConnection(opCtx, name) == false);
}

Status AuthzManagerExternalStateMerizod::query(
    OperationContext* opCtx,
    const NamespaceString& collectionName,
    const BSONObj& query,
    const BSONObj& projection,
    const stdx::function<void(const BSONObj&)>& resultProcessor) {
    try {
        DBDirectClient client(opCtx);
        client.query(resultProcessor, collectionName, query, &projection);
        return Status::OK();
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

Status AuthzManagerExternalStateMerizod::findOne(OperationContext* opCtx,
                                                const NamespaceString& collectionName,
                                                const BSONObj& query,
                                                BSONObj* result) {
    AutoGetCollectionForReadCommand ctx(opCtx, collectionName);

    BSONObj found;
    if (Helpers::findOne(opCtx, ctx.getCollection(), query, found)) {
        *result = found.getOwned();
        return Status::OK();
    }
    return Status(ErrorCodes::NoMatchingDocument,
                  merizoutils::str::stream() << "No document in " << collectionName.ns()
                                            << " matches "
                                            << query);
}

MERIZO_REGISTER_SHIM(AuthzManagerExternalState::create)
()->std::unique_ptr<AuthzManagerExternalState> {
    return std::make_unique<AuthzManagerExternalStateMerizod>();
}

}  // namespace merizo
