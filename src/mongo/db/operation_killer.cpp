// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/operation_killer.h"

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_key_manager.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <boost/optional/optional.hpp>

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

bool OperationKiller::isAuthorizedToKill(const ClientLock& target) const {
    AuthorizationSession* authzSession = AuthorizationSession::get(_myClient);

    if (target && authzSession->isCoauthorizedWithClient(&*target, target)) {
        return true;
    }

    return false;
}

void OperationKiller::killOperation(OperationId opId, ErrorCodes::Error killCode) {
    auto serviceContext = _myClient->getServiceContext();

    auto target = serviceContext->getLockedClient(opId);
    if (!target) {
        // There is no operation for opId
        return;
    }

    if (!isGenerallyAuthorizedToKill() && !isAuthorizedToKill(target)) {
        return;
    }

    auto opCtx = target->getOperationContext();
    if (opCtx->isKillOpsExempt()) {
        LOGV2_DEBUG(11227300, 3, "Not killing exempt op", "opId"_attr = opId);
        return;
    }

    serviceContext->killOperation(target, opCtx, killCode);

    LOGV2(20884, "Killed operation", "opId"_attr = opId);
}

void OperationKiller::killOperation(const OperationKey& opKey, ErrorCodes::Error killCode) {
    auto opId = OperationKeyManager::get(_myClient).at(opKey);

    if (!opId) {
        // There is no operation for opKey
        return;
    }

    killOperation(*opId, killCode);
}

}  // namespace mongo
