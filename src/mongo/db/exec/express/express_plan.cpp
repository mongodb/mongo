// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/express/express_plan.h"

#include "mongo/bson/bson_validate.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

MONGO_FAIL_POINT_DEFINE(throwWriteConflictExceptionInExpressWrite);
MONGO_FAIL_POINT_DEFINE(throwTemporarilyUnavailableExceptionInExpressWrite);

namespace express {
using namespace std::literals::string_view_literals;

void throwIfExpressWriteConflictFailpointEnabled() {
    if (MONGO_unlikely(throwWriteConflictExceptionInExpressWrite.shouldFail())) {
        throwWriteConflictException("Failpoint: throwWriteConflictExceptionInExpressWrite");
    }
}

void throwIfExpressTemporarilyUnavailableFailpointEnabled() {
    if (MONGO_unlikely(throwTemporarilyUnavailableExceptionInExpressWrite.shouldFail())) {
        throwTemporarilyUnavailableException(
            "Failpoint: throwTemporarilyUnavailableExceptionInExpressWrite");
    }
}

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

void assertFetchedRecordIsValidBson(const char* data,
                                    int size,
                                    const NamespaceString& ns,
                                    const RecordId& rid) {
    auto status = validateBSON(data, static_cast<uint64_t>(size));
    uassert(ErrorCodes::InvalidBSON,
            str::stream() << "Invalid BSON fetched from storage for EXPRESS update on "
                          << ns.toStringForErrorMsg() << " at RecordId " << rid.toString() << ": "
                          << status.reason(),
            status.isOK());
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
    builder.append("key"sv, redact(indexKey));
    builder.append("pattern"sv, keyPattern);
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
