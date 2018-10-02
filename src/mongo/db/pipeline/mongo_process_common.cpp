/**
 * Copyright (C) 2018 MongoDB Inc.
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

#include "mongo/db/pipeline/mongo_process_common.h"

#include "mongo/bson/mutable/document.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/service_context.h"

namespace mongo {

std::vector<BSONObj> MongoProcessCommon::getCurrentOps(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    CurrentOpConnectionsMode connMode,
    CurrentOpSessionsMode sessionMode,
    CurrentOpUserMode userMode,
    CurrentOpTruncateMode truncateMode,
    CurrentOpCursorMode cursorMode) const {
    OperationContext* opCtx = expCtx->opCtx;
    AuthorizationSession* ctxAuth = AuthorizationSession::get(opCtx->getClient());

    std::vector<BSONObj> ops;

    for (ServiceContext::LockedClientsCursor cursor(opCtx->getClient()->getServiceContext());
         Client* client = cursor.next();) {
        invariant(client);

        stdx::lock_guard<Client> lk(*client);

        // If auth is disabled, ignore the allUsers parameter.
        if (ctxAuth->getAuthorizationManager().isAuthEnabled() &&
            userMode == CurrentOpUserMode::kExcludeOthers &&
            !ctxAuth->isCoauthorizedWithClient(client)) {
            continue;
        }

        // Ignore inactive connections unless 'idleConnections' is true.
        if (!client->getOperationContext() && connMode == CurrentOpConnectionsMode::kExcludeIdle) {
            continue;
        }

        // Delegate to the mongoD- or mongoS-specific implementation of _reportCurrentOpForClient.
        ops.emplace_back(_reportCurrentOpForClient(opCtx, client, truncateMode));
    }

    // If 'cursorMode' is set to include idle cursors, retrieve them and add them to ops.
    if (cursorMode == CurrentOpCursorMode::kIncludeCursors) {

        for (auto&& cursor : getIdleCursors(expCtx, userMode)) {
            BSONObjBuilder cursorObj;
            cursorObj.append("type", "idleCursor");
            // On mongos, planSummary is not present.
            auto planSummaryData = cursor.getPlanSummary();
            if (planSummaryData) {
                auto planSummaryText = planSummaryData->toString();
                // Plan summary has to appear in the top level object, not the cursor object.
                // We remove it, create the op, then put it back.
                cursor.setPlanSummary(boost::none);
                cursorObj.append("planSummary", planSummaryText);
                cursorObj.append("cursor", cursor.toBSON());
                cursor.setPlanSummary(StringData(planSummaryText));
            } else {
                cursorObj.append("cursor", cursor.toBSON());
            }
            ops.emplace_back(cursorObj.obj());
        }
    }

    // If we need to report on idle Sessions, defer to the mongoD or mongoS implementations.
    if (sessionMode == CurrentOpSessionsMode::kIncludeIdle) {
        _reportCurrentOpsForIdleSessions(opCtx, userMode, &ops);
    }

    return ops;
}

bool MongoProcessCommon::keyPatternNamesExactPaths(const BSONObj& keyPattern,
                                                   const std::set<FieldPath>& uniqueKeyPaths) {
    size_t nFieldsMatched = 0;
    for (auto&& elem : keyPattern) {
        if (!elem.isNumber()) {
            return false;
        }
        if (uniqueKeyPaths.find(elem.fieldNameStringData()) == uniqueKeyPaths.end()) {
            return false;
        }
        ++nFieldsMatched;
    }
    return nFieldsMatched == uniqueKeyPaths.size();
}

}  // namespace mongo
