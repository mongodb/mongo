/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/exec/express/express_plan.h"

#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/util/stacktrace.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace express {

void releaseShardFilterResources(ScopedCollectionFilter&) {}
void restoreShardFilterResources(ScopedCollectionFilter&) {}

void releaseShardFilterResources(NoShardFilter&) {}
void restoreShardFilterResources(NoShardFilter&) {}

void releaseShardFilterResources(write_stage_common::PreWriteFilter& preWriteFilter) {
    preWriteFilter.saveState();
}

void restoreShardFilterResources(write_stage_common::PreWriteFilter& preWriteFilter) {
    preWriteFilter.restoreState();
}

void logRecordNotFound(OperationContext* opCtx,
                       const RecordId& rid,
                       const BSONObj& indexKey,
                       const BSONObj& keyPattern,
                       const NamespaceString& ns) {
    // The express path does not yield between examining the index entry and fetching it, so
    // it's not possible that the document was deleted during a yield after we first examined
    // the index entry. It is possible that the record was deleted by a prepared transaction
    // (race between document deletion & fetch). If we're ignoring prepare conflicts, then we
    // simply return here. Otherwise, we return an error and write to the log with instructions
    // on how to address potentially inconsistent data.
    if (shard_role_details::getRecoveryUnit(opCtx)->getPrepareConflictBehavior() !=
        PrepareConflictBehavior::kEnforce) {
        return;
    }

    auto options = [&] {
        if (shard_role_details::getRecoveryUnit(opCtx)->getDataCorruptionDetectionMode() ==
            DataCorruptionDetectionMode::kThrow) {
            return logv2::LogOptions{logv2::UserAssertAfterLog(ErrorCodes::DataCorruptionDetected)};
        } else {
            return logv2::LogOptions(logv2::LogComponent::kAutomaticDetermination);
        }
    }();

    BSONObjBuilder builder;
    builder.append("key"_sd, redact(indexKey));
    builder.append("pattern"_sd, keyPattern);
    const BSONObj indexKeyData = builder.obj();
    LOGV2_ERROR_OPTIONS(
        8944500,
        options,
        "Erroneous index key found with reference to non-existent record id. Consider dropping "
        "and then re-creating the index and then running the validate command on the "
        "collection.",
        logAttrs(ns),
        "recordId"_attr = rid,
        "indexKeyData"_attr = redact(indexKeyData));
}

}  // namespace express
}  // namespace mongo
