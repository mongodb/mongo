/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/logical_session_id.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/operation_context.h"

namespace mongo {

namespace {

/**
 * This is a safe hash that will not collide with a username because all full usernames include an
 * '@' character.
 */
const auto kNoAuthDigest = SHA256Block::computeHash(reinterpret_cast<const uint8_t*>(""), 0);

SHA256Block lookupUserDigest(OperationContext* opCtx) {
    auto client = opCtx->getClient();
    ServiceContext* serviceContext = client->getServiceContext();

    if (AuthorizationManager::get(serviceContext)->isAuthEnabled()) {
        UserName userName;

        auto authzSession = AuthorizationSession::get(client);
        auto userNameItr = authzSession->getAuthenticatedUserNames();
        if (userNameItr.more()) {
            userName = userNameItr.next();
            if (userNameItr.more()) {
                uasserted(ErrorCodes::Unauthorized,
                          "must only be authenticated as exactly one user "
                          "to create a logical session");
            }
        } else {
            uasserted(ErrorCodes::Unauthorized,
                      "must only be authenticated as exactly one user "
                      "to create a logical session");
        }

        User* user = authzSession->lookupUser(userName);
        invariant(user);

        return user->getDigest();
    } else {
        return kNoAuthDigest;
    }
}

}  // namespace

LogicalSessionId makeLogicalSessionId(const LogicalSessionFromClient& fromClient,
                                      OperationContext* opCtx) {
    LogicalSessionId lsid;

    lsid.setId(fromClient.getId());

    if (fromClient.getUid()) {
        auto authSession = AuthorizationSession::get(opCtx->getClient());

        uassert(ErrorCodes::Unauthorized,
                "Unauthorized to set user digest in LogicalSessionId",
                authSession->isAuthorizedForPrivilege(
                    Privilege(ResourcePattern::forClusterResource(), ActionType::impersonate)));

        lsid.setUid(*fromClient.getUid());
    } else {
        lsid.setUid(lookupUserDigest(opCtx));
    }

    return lsid;
}

LogicalSessionId makeLogicalSessionId(OperationContext* opCtx) {
    LogicalSessionId id{};

    id.setId(UUID::gen());
    id.setUid(lookupUserDigest(opCtx));

    return id;
}

LogicalSessionRecord makeLogicalSessionRecord(const LogicalSessionId& lsid, Date_t lastUse) {
    LogicalSessionRecord lsr;

    lsr.setId(lsid);
    lsr.setLastUse(lastUse);

    return lsr;
}

LogicalSessionToClient makeLogicalSessionToClient(const LogicalSessionId& lsid) {
    LogicalSessionToClient id;
    id.setId(lsid.getId());
    id.setTimeoutMinutes(localLogicalSessionTimeoutMinutes);

    return id;
};

void initializeOperationSessionInfo(OperationContext* opCtx,
                                    const BSONObj& requestBody,
                                    bool requiresAuth) {
    if (!requiresAuth) {
        return;
    }

    auto osi = OperationSessionInfoFromClient::parse(IDLParserErrorContext("OperationSessionInfo"),
                                                     requestBody);

    if (osi.getSessionId()) {
        opCtx->setLogicalSessionId(makeLogicalSessionId(*(osi.getSessionId()), opCtx));
    }

    if (osi.getTxnNumber()) {
        uassert(ErrorCodes::IllegalOperation,
                "Transaction number requires a sessionId to be specified",
                opCtx->getLogicalSessionId());
        uassert(ErrorCodes::BadValue,
                "Transaction number cannot be negative",
                *osi.getTxnNumber() >= 0);

        opCtx->setTxnNumber(*osi.getTxnNumber());
    }
}

}  // namespace mongo
