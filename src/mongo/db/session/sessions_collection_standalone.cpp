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

#include "mongo/db/session/sessions_collection_standalone.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/get_status_from_command_result.h"

namespace mongo {

namespace {

BSONObj lsidQuery(const LogicalSessionId& lsid) {
    return BSON(LogicalSessionRecord::kIdFieldName << lsid.toBSON());
}
}  // namespace

void SessionsCollectionStandalone::setupSessionsCollection(OperationContext* opCtx) {
    try {
        checkSessionsCollectionExists(opCtx);
    } catch (DBException& ex) {

        DBDirectClient client(opCtx);
        BSONObj cmd;

        if (ex.code() == ErrorCodes::IndexOptionsConflict) {
            cmd = generateCollModCmd();
        } else {
            cmd = generateCreateIndexesCmd();
        }

        BSONObj info;
        if (!client.runCommand(NamespaceString::kLogicalSessionsNamespace.dbName(), cmd, info)) {
            uassertStatusOKWithContext(
                getStatusFromCommandResult(info),
                str::stream() << "Failed to create "
                              << NamespaceString::kLogicalSessionsNamespace.toStringForErrorMsg());
        }
    }
}

void SessionsCollectionStandalone::checkSessionsCollectionExists(OperationContext* opCtx) {
    DBDirectClient client(opCtx);

    const bool includeBuildUUIDs = false;
    const int options = 0;
    auto indexes = client.getIndexSpecs(
        NamespaceString::kLogicalSessionsNamespace, includeBuildUUIDs, options);

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << NamespaceString::kLogicalSessionsNamespace.toStringForErrorMsg()
                          << " does not exist",
            indexes.size() != 0u);

    auto index = std::find_if(indexes.begin(), indexes.end(), [](const BSONObj& index) {
        return index.getField("name").String() == kSessionsTTLIndex;
    });

    uassert(ErrorCodes::IndexNotFound,
            str::stream() << NamespaceString::kLogicalSessionsNamespace.toStringForErrorMsg()
                          << " does not have the required TTL index",
            index != indexes.end());

    uassert(ErrorCodes::IndexOptionsConflict,
            str::stream() << NamespaceString::kLogicalSessionsNamespace.toStringForErrorMsg()
                          << " currently has the incorrect timeout for the TTL index",
            index->hasField("expireAfterSeconds") &&
                index->getField("expireAfterSeconds").Int() ==
                    (localLogicalSessionTimeoutMinutes * 60));
}

void SessionsCollectionStandalone::refreshSessions(OperationContext* opCtx,
                                                   const LogicalSessionRecordSet& sessions) {
    const std::vector<LogicalSessionRecord> sessionsVector(sessions.begin(), sessions.end());
    DBDirectClient client(opCtx);
    _doRefresh(NamespaceString::kLogicalSessionsNamespace,
               sessionsVector,
               makeSendFnForBatchWrite(NamespaceString::kLogicalSessionsNamespace, &client));
}

void SessionsCollectionStandalone::removeRecords(OperationContext* opCtx,
                                                 const LogicalSessionIdSet& sessions) {
    const std::vector<LogicalSessionId> sessionsVector(sessions.begin(), sessions.end());
    DBDirectClient client(opCtx);
    _doRemove(NamespaceString::kLogicalSessionsNamespace,
              sessionsVector,
              makeSendFnForBatchWrite(NamespaceString::kLogicalSessionsNamespace, &client));
}

LogicalSessionIdSet SessionsCollectionStandalone::findRemovedSessions(
    OperationContext* opCtx, const LogicalSessionIdSet& sessions) {
    const std::vector<LogicalSessionId> sessionsVector(sessions.begin(), sessions.end());
    DBDirectClient client(opCtx);
    return _doFindRemoved(
        NamespaceString::kLogicalSessionsNamespace,
        sessionsVector,
        makeFindFnForCommand(NamespaceString::kLogicalSessionsNamespace, &client));
}

}  // namespace mongo
