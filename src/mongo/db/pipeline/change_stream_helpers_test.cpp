// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/change_stream_helpers.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <array>

namespace mongo {
namespace {

using namespace change_stream;
using namespace std::literals::string_view_literals;

class ChangeStreamHelpersTest : public AggregationContextFixture {
protected:
    // A change event carrying its resume token as '_id' and single-element sort key, plus the
    // optional identity fields, mirroring what the transform produces.
    static Document eventDoc(const Value& token,
                             const Value& clusterTime = Value(),
                             const Value& wallTime = Value(),
                             const Value& ns = Value(),
                             const Value& documentKey = Value()) {
        MutableDocument md;
        md.addField(DocumentSourceChangeStream::kIdField, token);
        if (!clusterTime.missing()) {
            md.addField(DocumentSourceChangeStream::kClusterTimeField, clusterTime);
        }
        if (!wallTime.missing()) {
            md.addField(DocumentSourceChangeStream::kWallTimeField, wallTime);
        }
        if (!ns.missing()) {
            md.addField(DocumentSourceChangeStream::kNamespaceField, ns);
        }
        if (!documentKey.missing()) {
            md.addField(DocumentSourceChangeStream::kDocumentKeyField, documentKey);
        }
        md.metadata().setSortKey(token, true /* isSingleElementKey */);
        return md.freeze();
    }

    static Document errorEvent(ChangeStreamErrorSource source,
                               const Document& doc,
                               boost::optional<ShardId> shardId = boost::none) {
        try {
            uasserted(ErrorCodes::TypeMismatch, "stage error");
        } catch (const DBException& ex) {
            return makeChangeStreamErrorEvent(ex, source, doc, std::move(shardId));
        }
        MONGO_UNREACHABLE;
    }
};

TEST_F(ChangeStreamHelpersTest, ErrorEventShape) {
    const Value token(Document{{"_id", 42}});
    for (auto source :
         {ChangeStreamErrorSource::kUserPipeline, ChangeStreamErrorSource::kInternalPipeline}) {
        const auto doc = errorEvent(source,
                                    eventDoc(token,
                                             Value(Timestamp(100, 1)),
                                             Value(Date_t::fromMillisSinceEpoch(9000)),
                                             Value(Document{{"db", "d"sv}, {"coll", "c"sv}}),
                                             Value(Document{{"_id", 42}})));
        ASSERT_EQ(doc[DocumentSourceChangeStream::kOperationTypeField].getStringData(),
                  DocumentSourceChangeStream::kErrorOpType);
        ASSERT_EQ(doc[DocumentSourceChangeStream::kErrorField]
                     [DocumentSourceChangeStream::kErrorSourceField]
                         .getStringData(),
                  toString(source));
        ASSERT_VALUE_EQ(doc[DocumentSourceChangeStream::kIdField], token);
        ASSERT_VALUE_EQ(doc.metadata().getSortKey(), token);
        ASSERT_EQ(doc[DocumentSourceChangeStream::kErrorField]
                     [DocumentSourceChangeStream::kErrorCodeField]
                         .getInt(),
                  static_cast<int>(ErrorCodes::TypeMismatch));
        ASSERT_EQ(doc[DocumentSourceChangeStream::kErrorField]
                     [DocumentSourceChangeStream::kErrorCodeNameField]
                         .getStringData(),
                  "TypeMismatch");
        ASSERT_EQ(doc[DocumentSourceChangeStream::kErrorField]
                     [DocumentSourceChangeStream::kErrorMessageField]
                         .getStringData(),
                  "stage error");
        for (auto field : {DocumentSourceChangeStream::kClusterTimeField,
                           DocumentSourceChangeStream::kWallTimeField,
                           DocumentSourceChangeStream::kNamespaceField,
                           DocumentSourceChangeStream::kDocumentKeyField}) {
            ASSERT_FALSE(doc[field].missing()) << field;
        }
    }

    // Optional identity fields are omitted when the event does not carry them.
    const auto bare = errorEvent(ChangeStreamErrorSource::kUserPipeline, eventDoc(token));
    for (auto field : {DocumentSourceChangeStream::kClusterTimeField,
                       DocumentSourceChangeStream::kWallTimeField,
                       DocumentSourceChangeStream::kNamespaceField,
                       DocumentSourceChangeStream::kDocumentKeyField}) {
        ASSERT_TRUE(bare[field].missing()) << field;
    }
}

// Without a sort key the failure cannot be attributed to an event: the original error propagates.
TEST_F(ChangeStreamHelpersTest, ErrorEventFromDocumentWithoutSortKeyRethrows) {
    const Document doc{{DocumentSourceChangeStream::kIdField, Value(Document{{"_id", 1}})}};
    try {
        uasserted(ErrorCodes::TypeMismatch, "stage error");
    } catch (const DBException& ex) {
        ASSERT_THROWS_CODE(
            makeChangeStreamErrorEvent(ex, ChangeStreamErrorSource::kUserPipeline, doc),
            AssertionException,
            ErrorCodes::TypeMismatch);
    }
}

// An event whose '_id' a user stage rewrote must not yield an error event carrying the user's
// value as its resume token: fail closed with ChangeStreamFatalError, exactly as delivering the
// event itself would.
TEST_F(ChangeStreamHelpersTest, ErrorEventFailsClosedWhenIdWasModified) {
    const Value token(Document{{"_id", 42}});
    const Document doc = eventDoc(token, Value(Timestamp(100, 1)));
    MutableDocument rewritten(doc);
    rewritten[DocumentSourceChangeStream::kIdField] = Value(Document{{"a", 1}});

    try {
        uasserted(ErrorCodes::TypeMismatch, "stage error");
    } catch (const DBException& ex) {
        ASSERT_THROWS_CODE(makeChangeStreamErrorEvent(
                               ex, ChangeStreamErrorSource::kUserPipeline, rewritten.freeze()),
                           AssertionException,
                           ErrorCodes::ChangeStreamFatalError);
    }
}

TEST_F(ChangeStreamHelpersTest, ErrorEventIncludesShardIdWhenPresent) {
    const Value token(Document{{"_id", 42}});
    try {
        uasserted(ErrorCodes::TypeMismatch, "stage error");
    } catch (const DBException& ex) {
        // An error that originated on a shard records the shard, like a resume-gap event.
        const auto withShard = makeChangeStreamErrorEvent(
            ex, ChangeStreamErrorSource::kInternalPipeline, eventDoc(token), ShardId("shard0"));
        ASSERT_EQ(withShard[DocumentSourceChangeStream::kErrorField]
                           [DocumentSourceChangeStream::kShardIdField]
                               .getStringData(),
                  "shard0");

        // No shard (mongos, or a non-sharded replica set): the field is omitted.
        const auto without =
            makeChangeStreamErrorEvent(ex, ChangeStreamErrorSource::kUserPipeline, eventDoc(token));
        ASSERT_TRUE(without[DocumentSourceChangeStream::kErrorField]
                           [DocumentSourceChangeStream::kShardIdField]
                               .missing());
    }
}

TEST_F(ChangeStreamHelpersTest, IsConvertible) {
    for (auto code : {ErrorCodes::Interrupted,
                      ErrorCodes::InterruptedAtShutdown,
                      ErrorCodes::NotWritablePrimary,
                      ErrorCodes::HostUnreachable,
                      ErrorCodes::StaleConfig,
                      ErrorCodes::StaleEpoch,
                      ErrorCodes::MaxTimeMSExpired,
                      ErrorCodes::QueryPlanKilled,
                      ErrorCodes::ChangeStreamFatalError,
                      ErrorCodes::ShardRemovedError,
                      ErrorCodes::RetryChangeStream,
                      ErrorCodes::ChangeStreamInvalidated,
                      ErrorCodes::ChangeStreamStartAfterInvalidate,
                      // Server-internal failures must not be surfaced to the client as events.
                      ErrorCodes::InternalError,
                      ErrorCodes::QueryTrialRunCompleted,        // InternalOnly
                      ErrorCodes::ChangeStreamErrorEventBypass,  // InternalOnly
                      ErrorCodes::RateLimitExceeded}) {          // SystemOverloadedError
        ASSERT_FALSE(isConvertibleChangeStreamError(code)) << code;
    }
    for (auto code : {ErrorCodes::TypeMismatch,
                      ErrorCodes::BadValue,
                      ErrorCodes::NoMatchingDocument,
                      ErrorCodes::BSONObjectTooLarge,
                      ErrorCodes::ChangeStreamHistoryLost,
                      ErrorCodes::ChangeStreamResumedTokenFilteredOut}) {
        ASSERT_TRUE(isConvertibleChangeStreamError(code)) << code;
    }
}

// Transient infrastructure failures (stale routing, snapshot, write conflict, lock, shard
// availability, oplog position, cancellation) must propagate even with showErrorsAsEvents:true, so
// that a resume delivers the event, rather than be converted into an error event that skips it.
TEST_F(ChangeStreamHelpersTest, TransientErrorsAreNotConvertible) {
    for (auto code : {ErrorCodes::StaleDbVersion,
                      ErrorCodes::SnapshotTooOld,
                      ErrorCodes::SnapshotUnavailable,
                      ErrorCodes::MigrationConflict,
                      ErrorCodes::StaleChunkHistory,
                      ErrorCodes::CallbackCanceled,
                      ErrorCodes::ShardNotFound,
                      ErrorCodes::MovePrimaryInProgress,
                      ErrorCodes::ShardCannotRefreshDueToLocksHeld,
                      ErrorCodes::FailedToSatisfyReadPreference,
                      ErrorCodes::WriteConflict,
                      ErrorCodes::TemporarilyUnavailable,
                      ErrorCodes::LockBusy,
                      ErrorCodes::CappedPositionLost,
                      ErrorCodes::OplogQueryMinTsMissing,
                      ErrorCodes::NotWritablePrimary,
                      ErrorCodes::ShutdownInProgress,
                      ErrorCodes::StaleConfig}) {
        ASSERT_TRUE(isTransientChangeStreamError(code)) << code;
        ASSERT_FALSE(isConvertibleChangeStreamError(code)) << code;
    }
    // Document-scoped failures, and the resume-gap codes, are neither transient nor excluded.
    for (auto code : {ErrorCodes::TypeMismatch,
                      ErrorCodes::BadValue,
                      ErrorCodes::ConversionFailure,
                      ErrorCodes::BSONObjectTooLarge,
                      ErrorCodes::ChangeStreamHistoryLost,
                      ErrorCodes::ChangeStreamResumedTokenFilteredOut}) {
        ASSERT_FALSE(isTransientChangeStreamError(code)) << code;
        ASSERT_TRUE(isConvertibleChangeStreamError(code)) << code;
    }
}


// No infrastructure/transient error code may be converted into an error event.
TEST_F(ChangeStreamHelpersTest, NoInfrastructureOrTransientErrorIsConvertible) {
    const std::array<ErrorCodes::Error, 83> nonConvertible = {
        ErrorCodes::AuthenticationAbandoned,
        ErrorCodes::BackupCursorOpenConflictWithCheckpoint,
        ErrorCodes::CallbackCanceled,
        ErrorCodes::CappedPositionLost,
        ErrorCodes::ChangeStreamFatalError,
        ErrorCodes::ChangeStreamInvalidated,
        ErrorCodes::ChangeStreamStartAfterInvalidate,
        ErrorCodes::ClientDisconnect,
        ErrorCodes::ClientMarkedKilled,
        ErrorCodes::CloseConnectionForShutdownCommand,
        ErrorCodes::ConnectionError,
        ErrorCodes::ConnectionPoolExpired,
        ErrorCodes::CursorKilled,
        ErrorCodes::CursorNotFound,
        ErrorCodes::DatabaseMetadataRefreshCanceled,
        ErrorCodes::ExceededTimeLimit,
        ErrorCodes::FailedToSatisfyReadPreference,
        ErrorCodes::HostNotFound,
        ErrorCodes::HostUnreachable,
        ErrorCodes::IngressRequestRateLimitExceeded,
        ErrorCodes::InternalError,
        ErrorCodes::InternalTransactionNotSupported,
        ErrorCodes::Interrupted,
        ErrorCodes::InterruptedAtShutdown,
        ErrorCodes::InterruptedDueToReplStateChange,
        ErrorCodes::InterruptedDueToStorageChange,
        ErrorCodes::LockBusy,
        ErrorCodes::LockTimeout,
        ErrorCodes::MaxTimeMSExpired,
        ErrorCodes::MigrationBlockingOperationCoordinatorCleaningUp,
        ErrorCodes::MigrationConflict,
        ErrorCodes::MovePrimaryInProgress,
        ErrorCodes::NetworkInterfaceExceededTimeLimit,
        ErrorCodes::NetworkTimeout,
        ErrorCodes::NotPrimaryNoSecondaryOk,
        ErrorCodes::NotPrimaryOrSecondary,
        ErrorCodes::NotWritablePrimary,
        ErrorCodes::ObjectIsBusy,
        ErrorCodes::OpCtxKilledOnMarkKillOpsExempt,
        ErrorCodes::OperationFailed,
        ErrorCodes::OperationIsKilledAndDelisted,
        ErrorCodes::OplogQueryMinTsMissing,
        ErrorCodes::PeriodicJobIsStopped,
        ErrorCodes::PlacementVersionRefreshCanceled,
        ErrorCodes::PooledConnectionAcquisitionExceededTimeLimit,
        ErrorCodes::PrimarySteppedDown,
        ErrorCodes::QueryPlanKilled,
        ErrorCodes::QueryTrialRunCompleted,
        ErrorCodes::RateLimitExceeded,
        ErrorCodes::ReadConcernMajorityNotAvailableYet,
        ErrorCodes::ReadThroughCacheLookupCanceled,
        ErrorCodes::RemoteCommandExecutionError,
        ErrorCodes::ReshardingCoordinatorServiceConflictingOperationInProgress,
        ErrorCodes::ResourceExhausted,
        ErrorCodes::RetriableRemoteCommandFailure,
        ErrorCodes::RetryChangeStream,
        ErrorCodes::SearchRequestRejectedDueToOverload,
        ErrorCodes::ServiceExecutorInShutdown,
        ErrorCodes::ShardCannotRefreshDueToLocksHeld,
        ErrorCodes::ShardNotFound,
        ErrorCodes::ShutdownInProgress,
        ErrorCodes::SnapshotTooOld,
        ErrorCodes::SnapshotUnavailable,
        ErrorCodes::SocketException,
        ErrorCodes::StaleChunkHistory,
        ErrorCodes::StaleConfig,
        ErrorCodes::StaleDbVersion,
        ErrorCodes::StaleEpoch,
        ErrorCodes::TemporarilyUnavailable,
        ErrorCodes::TimeseriesBucketCleared,
        ErrorCodes::TimeseriesBucketFrozen,
        ErrorCodes::TooManyOpenTransactions,
        ErrorCodes::TooStaleToSyncFromSource,
        ErrorCodes::TransactionAPIMustRetryCommit,
        ErrorCodes::TransactionAPIMustRetryTransaction,
        ErrorCodes::TransactionCoordinatorCanceled,
        ErrorCodes::TransactionCoordinatorDeadlineTaskCanceled,
        ErrorCodes::TransactionCoordinatorReachedAbortDecision,
        ErrorCodes::TransactionCoordinatorSteppingDown,
        ErrorCodes::TransactionExceededLifetimeLimitSeconds,
        ErrorCodes::UpdatesStillPending,
        ErrorCodes::WaitForMajorityServiceEarlierOpTimeAvailable,
        ErrorCodes::WriteConflict,
    };
    for (const auto code : nonConvertible) {
        ASSERT_FALSE(isConvertibleChangeStreamError(code))
            << "infrastructure/transient code " << code << " (" << ErrorCodes::errorString(code)
            << ") must not be convertible";
    }
}

}  // namespace
}  // namespace mongo
