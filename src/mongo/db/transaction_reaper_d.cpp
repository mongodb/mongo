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

#include "mongo/platform/basic.h"

#include "mongo/db/transaction_reaper_d.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/sessions_collection.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

const auto kIdProjection = BSON(SessionTxnRecord::kSessionIdFieldName << 1);
const auto kSortById = BSON(SessionTxnRecord::kSessionIdFieldName << 1);
const auto kLastWriteDateFieldName = SessionTxnRecord::kLastWriteDateFieldName;

/**
 * Removes the specified set of session ids from the persistent sessions collection and returns the
 * number of sessions actually removed.
 */
int removeSessionsTransactionRecords(OperationContext* opCtx,
                                     SessionsCollection& sessionsCollection,
                                     const LogicalSessionIdSet& sessionIdsToRemove) {
    if (sessionIdsToRemove.empty()) {
        return 0;
    }

    // From the passed-in sessions, find the ones which are actually expired/removed
    auto expiredSessionIds =
        uassertStatusOK(sessionsCollection.findRemovedSessions(opCtx, sessionIdsToRemove));

    write_ops::Delete deleteOp(NamespaceString::kSessionTransactionsTableNamespace);
    deleteOp.setWriteCommandBase([] {
        write_ops::WriteCommandBase base;
        base.setOrdered(false);
        return base;
    }());
    deleteOp.setDeletes([&] {
        std::vector<write_ops::DeleteOpEntry> entries;
        for (auto it = expiredSessionIds.begin(); it != expiredSessionIds.end(); ++it) {
            entries.emplace_back(BSON(LogicalSessionRecord::kIdFieldName << it->toBSON()),
                                 false /* multi = false */);
        }
        return entries;
    }());

    BSONObj result;

    DBDirectClient client(opCtx);
    client.runCommand(NamespaceString::kSessionTransactionsTableNamespace.db().toString(),
                      deleteOp.toBSON({}),
                      result);

    BatchedCommandResponse response;
    std::string errmsg;
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "Failed to parse response " << result,
            response.parseBSON(result, &errmsg));
    uassertStatusOK(response.getTopLevelStatus());

    return response.getN();
}

}  // namespace

int TransactionReaperD::reapSessionsOlderThan(OperationContext* opCtx,
                                              SessionsCollection& sessionsCollection,
                                              Date_t possiblyExpired) {
    // Scan for records older than the minimum lifetime and uses a sort to walk the '_id' index
    DBDirectClient client(opCtx);
    auto cursor =
        client.query(NamespaceString::kSessionTransactionsTableNamespace,
                     Query(BSON(kLastWriteDateFieldName << LT << possiblyExpired)).sort(kSortById),
                     0,
                     0,
                     &kIdProjection);

    // The max batch size is chosen so that a single batch won't exceed the 16MB BSON object
    // size limit
    LogicalSessionIdSet lsids;
    const int kMaxBatchSize = 10'000;

    int numReaped = 0;
    while (cursor->more()) {
        auto transactionSession = SessionsCollectionFetchResultIndividualResult::parse(
            "TransactionSession"_sd, cursor->next());

        lsids.insert(transactionSession.get_id());
        if (lsids.size() > kMaxBatchSize) {
            numReaped += removeSessionsTransactionRecords(opCtx, sessionsCollection, lsids);
            lsids.clear();
        }
    }

    numReaped += removeSessionsTransactionRecords(opCtx, sessionsCollection, lsids);

    return numReaped;
}

}  // namespace mongo
