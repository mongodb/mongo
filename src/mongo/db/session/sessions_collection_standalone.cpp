// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/session/sessions_collection_standalone.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session/logical_session_cache_gen.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <list>
#include <vector>

#include <absl/container/node_hash_map.h>

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

SessionsCollection::RefreshSessionsResult SessionsCollectionStandalone::refreshSessions(
    OperationContext* opCtx, const LogicalSessionRecordSet& sessions) {
    const std::vector<LogicalSessionRecord> sessionsVector(sessions.begin(), sessions.end());
    DBDirectClient client(opCtx);
    return _doRefresh(NamespaceString::kLogicalSessionsNamespace,
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
