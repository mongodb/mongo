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

#include "mongo/db/session/logical_session_id_helpers.h"

#include "mongo/base/data_range.h"
#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session/logical_session_cache_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {
/**
 * If auth is not enabled, will return boost::none.
 * Otherwise, a user must be actively authenticated on the client,
 * and a handle to this user will be returned.
 */
boost::optional<UserHandle> getAuthenticatedUser(Client* client) {
    if (!AuthorizationManager::get(client->getService())->isAuthEnabled()) {
        return boost::none;
    }

    auto optUser = AuthorizationSession::get(client)->getAuthenticatedUser();
    uassert(ErrorCodes::Unauthorized, "Logical sessions require authentication", optUser);

    return optUser.value();
}
}  // namespace

/**
 * This is a safe hash that will not collide with a username because all full usernames include an
 * '@' character.
 */
const auto kNoAuthDigest = SHA256Block::computeHash(reinterpret_cast<const uint8_t*>(""), 0);

SHA256Block getLogicalSessionUserDigestForLoggedInUser(const OperationContext* opCtx) {
    if (auto user = getAuthenticatedUser(opCtx->getClient())) {
        uassert(ErrorCodes::BadValue,
                "Username too long to use with logical sessions",
                user.value()->getName().getDisplayNameLength() <
                    kMaximumUserNameLengthForLogicalSessions);
        return user.value()->getDigest();
    } else {
        return kNoAuthDigest;
    }
}

SHA256Block getLogicalSessionUserDigestFor(StringData user, StringData db) {
    if (user.empty() && db.empty()) {
        return kNoAuthDigest;
    }
    const UserName un(user, db);
    auto fn = un.getDisplayName();
    return SHA256Block::computeHash({ConstDataRange(fn.c_str(), fn.size())});
}

bool isParentSessionId(const LogicalSessionId& sessionId) {
    // All child sessions must have a txnUUID.
    return !sessionId.getTxnUUID();
}

bool isChildSession(const LogicalSessionId& sessionId) {
    // All child sessions must have a txnUUID.
    return bool(sessionId.getTxnUUID());
}

boost::optional<LogicalSessionId> getParentSessionId(const LogicalSessionId& sessionId) {
    if (sessionId.getTxnUUID()) {
        return LogicalSessionId{sessionId.getId(), sessionId.getUid()};
    }
    return boost::none;
}

LogicalSessionId castToParentSessionId(const LogicalSessionId& sessionId) {
    if (auto parentSessionId = getParentSessionId(sessionId)) {
        return *parentSessionId;
    }
    return sessionId;
}

bool isInternalSessionForRetryableWrite(const LogicalSessionId& sessionId) {
    return sessionId.getTxnNumber().has_value();
}

bool isDefaultTxnRetryCounter(TxnRetryCounter txnRetryCounter) {
    return txnRetryCounter == 0;
}

bool isInternalSessionForNonRetryableWrite(const LogicalSessionId& sessionId) {
    return sessionId.getTxnUUID() && !sessionId.getTxnNumber();
}

LogicalSessionId makeLogicalSessionIdWithTxnNumberAndUUID(const LogicalSessionId& parentLsid,
                                                          TxnNumber txnNumber) {
    auto lsid = LogicalSessionId(parentLsid.getId(), parentLsid.getUid());
    lsid.setTxnNumber(txnNumber);
    lsid.setTxnUUID(UUID::gen());
    return lsid;
}

LogicalSessionId makeLogicalSessionIdWithTxnUUID(const LogicalSessionId& parentLsid) {
    auto lsid = LogicalSessionId(parentLsid.getId(), parentLsid.getUid());
    lsid.setTxnUUID(UUID::gen());
    return lsid;
}

LogicalSessionId makeLogicalSessionId(const LogicalSessionFromClient& fromClient,
                                      OperationContext* opCtx,
                                      std::initializer_list<Privilege> allowSpoof) {
    uassert(ErrorCodes::InvalidOptions,
            "Cannot specify txnNumber in lsid without specifying txnUUID",
            !fromClient.getTxnNumber() || fromClient.getTxnUUID());

    LogicalSessionId lsid;

    lsid.setId(fromClient.getId());
    lsid.setTxnNumber(fromClient.getTxnNumber());
    lsid.setTxnUUID(fromClient.getTxnUUID());

    if (fromClient.getUid()) {
        auto authSession = AuthorizationSession::get(opCtx->getClient());

        uassert(ErrorCodes::Unauthorized,
                "Unauthorized to set user digest in LogicalSessionId",
                std::any_of(allowSpoof.begin(),
                            allowSpoof.end(),
                            [&](const auto& priv) {
                                return authSession->isAuthorizedForPrivilege(priv);
                            }) ||
                    authSession->isAuthorizedForPrivilege(Privilege(
                        ResourcePattern::forClusterResource(authSession->getUserTenantId()),
                        ActionType::impersonate)) ||
                    getLogicalSessionUserDigestForLoggedInUser(opCtx) == fromClient.getUid());

        lsid.setUid(*fromClient.getUid());
    } else {
        lsid.setUid(getLogicalSessionUserDigestForLoggedInUser(opCtx));
    }

    return lsid;
}

LogicalSessionId makeLogicalSessionId(OperationContext* opCtx) {
    LogicalSessionId id{};

    id.setId(UUID::gen());
    id.setUid(getLogicalSessionUserDigestForLoggedInUser(opCtx));

    return id;
}

LogicalSessionId makeSystemLogicalSessionId() {
    LogicalSessionId id{};

    id.setId(UUID::gen());
    id.setUid((*internalSecurity.getUser())->getDigest());

    return id;
}

LogicalSessionRecord makeLogicalSessionRecord(OperationContext* opCtx, Date_t lastUse) {
    LogicalSessionId id{};
    LogicalSessionRecord lsr{};

    if (auto user = getAuthenticatedUser(opCtx->getClient())) {
        id.setUid(user.value()->getDigest());
        lsr.setUser(user.value()->getName().getDisplayName());
    } else {
        id.setUid(kNoAuthDigest);
    }

    id.setId(UUID::gen());

    lsr.setId(id);
    lsr.setLastUse(lastUse);

    return lsr;
}

LogicalSessionRecord makeLogicalSessionRecord(const LogicalSessionId& lsid, Date_t lastUse) {
    LogicalSessionId id{};
    LogicalSessionRecord lsr{};

    id.setId(lsid.getId());
    id.setUid(lsid.getUid());

    lsr.setId(id);
    lsr.setLastUse(lastUse);

    return lsr;
}

LogicalSessionRecord makeLogicalSessionRecord(OperationContext* opCtx,
                                              const LogicalSessionId& lsid,
                                              Date_t lastUse) {
    auto lsr = makeLogicalSessionRecord(lsid, lastUse);

    if (auto user = getAuthenticatedUser(opCtx->getClient())) {
        if (user.value()->getDigest() == lsid.getUid()) {
            lsr.setUser(user.value()->getName().getDisplayName());
        }
    }

    return lsr;
}


LogicalSessionToClient makeLogicalSessionToClient(const LogicalSessionId& lsid) {
    LogicalSessionIdToClient lsitc;
    lsitc.setId(lsid.getId());

    LogicalSessionToClient id;

    id.setId(lsitc);
    id.setTimeoutMinutes(localLogicalSessionTimeoutMinutes);

    return id;
};

LogicalSessionIdSet makeLogicalSessionIds(const std::vector<LogicalSessionFromClient>& sessions,
                                          OperationContext* opCtx,
                                          std::initializer_list<Privilege> allowSpoof) {
    LogicalSessionIdSet lsids;
    lsids.reserve(sessions.size());
    for (auto&& session : sessions) {
        lsids.emplace(makeLogicalSessionId(session, opCtx, allowSpoof));
    }

    return lsids;
}

namespace logical_session_id_helpers {

void serializeLsidAndTxnNumber(OperationContext* opCtx, BSONObjBuilder* builder) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(opCtx->getLogicalSessionId());
    sessionInfo.setTxnNumber(opCtx->getTxnNumber());
    sessionInfo.serialize(builder);
}

void serializeLsid(OperationContext* opCtx, BSONObjBuilder* builder) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(opCtx->getLogicalSessionId());
    sessionInfo.serialize(builder);
}

}  // namespace logical_session_id_helpers
}  // namespace mongo
