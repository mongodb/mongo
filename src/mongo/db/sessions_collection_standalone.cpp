
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

#include "mongo/db/sessions_collection_standalone.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/client/query.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/get_status_from_command_result.h"

namespace mongo {

namespace {

BSONObj lsidQuery(const LogicalSessionId& lsid) {
    return BSON(LogicalSessionRecord::kIdFieldName << lsid.toBSON());
}
}  // namespace

Status SessionsCollectionStandalone::setupSessionsCollection(OperationContext* opCtx) {
    auto existsStatus = checkSessionsCollectionExists(opCtx);
    if (existsStatus.isOK()) {
        return Status::OK();
    }

    DBDirectClient client(opCtx);
    BSONObj cmd;

    if (existsStatus.code() == ErrorCodes::IndexOptionsConflict) {
        cmd = generateCollModCmd();
    } else {
        cmd = generateCreateIndexesCmd();
    }

    BSONObj info;
    if (!client.runCommand(kSessionsNamespaceString.db().toString(), cmd, info)) {
        return getStatusFromCommandResult(info);
    }

    return Status::OK();
}

Status SessionsCollectionStandalone::checkSessionsCollectionExists(OperationContext* opCtx) {
    DBDirectClient client(opCtx);

    auto indexes = client.getIndexSpecs(kSessionsNamespaceString.toString());

    if (indexes.size() == 0u) {
        return Status{ErrorCodes::NamespaceNotFound, "config.system.sessions does not exist"};
    }

    auto index = std::find_if(indexes.begin(), indexes.end(), [](const BSONObj& index) {
        return index.getField("name").String() == kSessionsTTLIndex;
    });

    if (index == indexes.end()) {
        return Status{ErrorCodes::IndexNotFound,
                      "config.system.sessions does not have the required TTL index"};
    };

    if (!index->hasField("expireAfterSeconds") ||
        index->getField("expireAfterSeconds").Int() != (localLogicalSessionTimeoutMinutes * 60)) {
        return Status{
            ErrorCodes::IndexOptionsConflict,
            "config.system.sessions currently has the incorrect timeout for the TTL index"};
    }

    return Status::OK();
}

Status SessionsCollectionStandalone::refreshSessions(OperationContext* opCtx,
                                                     const LogicalSessionRecordSet& sessions) {
    DBDirectClient client(opCtx);
    return doRefresh(kSessionsNamespaceString,
                     sessions,
                     makeSendFnForBatchWrite(kSessionsNamespaceString, &client));
}

Status SessionsCollectionStandalone::removeRecords(OperationContext* opCtx,
                                                   const LogicalSessionIdSet& sessions) {
    DBDirectClient client(opCtx);
    return doRemove(kSessionsNamespaceString,
                    sessions,
                    makeSendFnForBatchWrite(kSessionsNamespaceString, &client));
}

StatusWith<LogicalSessionIdSet> SessionsCollectionStandalone::findRemovedSessions(
    OperationContext* opCtx, const LogicalSessionIdSet& sessions) {
    DBDirectClient client(opCtx);
    return doFetch(kSessionsNamespaceString,
                   sessions,
                   makeFindFnForCommand(kSessionsNamespaceString, &client));
}

}  // namespace mongo
