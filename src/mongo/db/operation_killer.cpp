/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include <boost/optional/optional.hpp>

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_key_manager.h"
#include "mongo/db/operation_killer.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

OperationKiller::OperationKiller(Client* myClient) : _myClient(myClient) {
    invariant(_myClient);
}

bool OperationKiller::isGenerallyAuthorizedToKill() const {
    AuthorizationSession* authzSession = AuthorizationSession::get(_myClient);
    return authzSession->isAuthorizedForActionsOnResource(
        ResourcePattern::forClusterResource(authzSession->getUserTenantId()), ActionType::killop);
}

bool OperationKiller::isAuthorizedToKill(const LockedClient& target) const {
    AuthorizationSession* authzSession = AuthorizationSession::get(_myClient);

    if (target && authzSession->isCoauthorizedWithClient(target.client(), target)) {
        return true;
    }

    return false;
}

void OperationKiller::killOperation(OperationId opId) {
    auto serviceContext = _myClient->getServiceContext();

    auto target = serviceContext->getLockedClient(opId);
    if (!target) {
        // There is no operation for opId
        return;
    }

    if (!isGenerallyAuthorizedToKill() && !isAuthorizedToKill(target)) {
        // The client is not authotized to kill this operation
        return;
    }

    serviceContext->killOperation(target, target->getOperationContext());

    LOGV2(20884, "Killed operation: {opId}", "Killed operation", "opId"_attr = opId);
}

void OperationKiller::killOperation(const OperationKey& opKey) {
    auto opId = OperationKeyManager::get(_myClient).at(opKey);

    if (!opId) {
        // There is no operation for opKey
        return;
    }

    killOperation(*opId);
}

}  // namespace mongo
