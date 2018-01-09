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

#include "mongo/db/initialize_operation_session_info.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"

namespace mongo {

void initializeOperationSessionInfo(OperationContext* opCtx,
                                    const BSONObj& requestBody,
                                    bool requiresAuth,
                                    bool isReplSetMemberOrMongos,
                                    bool supportsDocLocking) {
    if (!requiresAuth) {
        return;
    }

    {
        // If we're using the localhost bypass, and the client hasn't authenticated,
        // logical sessions are disabled. A client may authenticate as the __sytem user,
        // or as an externally authorized user.
        AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());
        if (authSession && authSession->isUsingLocalhostBypass() &&
            !authSession->getAuthenticatedUserNames().more()) {
            return;
        }
    }

    auto osi = OperationSessionInfoFromClient::parse("OperationSessionInfo"_sd, requestBody);

    if (osi.getSessionId()) {
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "cannot pass logical session id unless fully upgraded to "
                                 "featureCompatibilityVersion 3.6. See "
                              << feature_compatibility_version::kDochubLink
                              << " .",
                serverGlobalParams.featureCompatibility.getVersion() ==
                    ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36);

        stdx::lock_guard<Client> lk(*opCtx->getClient());

        opCtx->setLogicalSessionId(makeLogicalSessionId(osi.getSessionId().get(), opCtx));

        LogicalSessionCache* lsc = LogicalSessionCache::get(opCtx->getServiceContext());
        lsc->vivify(opCtx, opCtx->getLogicalSessionId().get());
    }

    if (osi.getTxnNumber()) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());

        uassert(ErrorCodes::IllegalOperation,
                "Transaction number requires a sessionId to be specified",
                opCtx->getLogicalSessionId());
        uassert(ErrorCodes::IllegalOperation,
                "Transaction numbers are only allowed on a replica set member or mongos",
                isReplSetMemberOrMongos);
        uassert(ErrorCodes::IllegalOperation,
                "Transaction numbers are only allowed on storage engines that support "
                "document-level locking",
                supportsDocLocking);
        uassert(ErrorCodes::BadValue,
                "Transaction number cannot be negative",
                *osi.getTxnNumber() >= 0);

        opCtx->setTxnNumber(*osi.getTxnNumber());
    }
}

}  // namespace mongo
