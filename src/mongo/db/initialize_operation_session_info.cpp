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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/initialize_operation_session_info.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/log.h"

namespace mongo {

Status initializeOperationSessionInfo(OperationContext* opCtx,
                                      const BSONObj& requestBody,
                                      bool requiresAuth,
                                      bool isReplSetMemberOrMongos,
                                      bool supportsDocLocking) {

    auto osi = OperationSessionInfoFromClient::parse("OperationSessionInfo"_sd, requestBody);

    if (opCtx->getClient()->isInDirectClient() && (osi.getSessionId() || osi.getTxnNumber())) {
        return Status(ErrorCodes::InvalidOptions,
                      "Invalid to set operation session info in a direct client");
    }

    if (!requiresAuth) {
        return Status::OK();
    }

    {
        // If we're using the localhost bypass, and the client hasn't authenticated,
        // logical sessions are disabled. A client may authenticate as the __sytem user,
        // or as an externally authorized user.
        AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());
        if (authSession && authSession->isUsingLocalhostBypass() &&
            !authSession->getAuthenticatedUserNames().more()) {
            return Status::OK();
        }
    }

    bool isFCV36 = (serverGlobalParams.featureCompatibility.getVersion() ==
                    ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36);

    // For the drivers it will look like the session is successfully added in FCV 3.4.
    if (osi.getSessionId() && isFCV36) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());

        opCtx->setLogicalSessionId(makeLogicalSessionId(osi.getSessionId().get(), opCtx));

        LogicalSessionCache* lsc = LogicalSessionCache::get(opCtx->getServiceContext());
        auto vivifyStatus = lsc->vivify(opCtx, opCtx->getLogicalSessionId().get());
        if (vivifyStatus != Status::OK()) {
            return vivifyStatus;
        }
    }

    if (osi.getTxnNumber()) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());

        if (!isFCV36) {
            return SessionsCommandFCV34Status("Retryable writes");
        }

        if (!opCtx->getLogicalSessionId()) {
            return {ErrorCodes::IllegalOperation,
                    "Transaction number requires a sessionId to be specified"};
        }

        if (!isReplSetMemberOrMongos) {
            return {ErrorCodes::IllegalOperation,
                    "Transaction numbers are only allowed on a replica set member or mongos"};
        }


        if (!supportsDocLocking) {
            return {ErrorCodes::IllegalOperation,
                    "Transaction numbers are only allowed on storage engines that support "
                    "document-level locking"};
        }

        if (*osi.getTxnNumber() < 0) {
            return {ErrorCodes::BadValue, "Transaction number cannot be negative"};
        }

        opCtx->setTxnNumber(*osi.getTxnNumber());
    }

    return Status::OK();
}

}  // namespace mongo
