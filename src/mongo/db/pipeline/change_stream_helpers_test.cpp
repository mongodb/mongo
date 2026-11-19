// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/change_stream_helpers.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
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
    static ResumeTokenData requestedToken() {
        static const UUID uuid = UUID::gen();
        return {Timestamp(100, 1), 0, 0, uuid, Value(Document{{"_id", "tok"sv}})};
    }

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

TEST_F(ChangeStreamHelpersTest, OversizedEventErrorUsesTheEventIdentity) {
    DocumentSourceChangeStreamSpec spec;
    spec.setShowErrorsAsEvents(true);
    getExpCtx()->setChangeStreamSpec(spec);

    // A change event carrying its resume token as _id and sort key.
    const auto tokenData = requestedToken();
    const Value token(ResumeToken(tokenData).toDocument());
    const auto event = eventDoc(
        token, Value(tokenData.clusterTime), Value(), Value(), Value(Document{{"_id", 7}}));

    try {
        uasserted(ErrorCodes::BSONObjectTooLarge, "event too large");
    } catch (const DBException& ex) {
        auto error = tryBuildOversizedEventError(ex, event, getExpCtx());
        ASSERT_TRUE(error.has_value());
        ASSERT_EQ(error
                      ->getField(DocumentSourceChangeStream::kErrorField)
                          [DocumentSourceChangeStream::kErrorSourceField]
                      .getStringData(),
                  kCsOversizedEventSource);
        ASSERT_EQ(error
                      ->getField(DocumentSourceChangeStream::kErrorField)
                          [DocumentSourceChangeStream::kErrorCodeField]
                      .getInt(),
                  static_cast<int>(ErrorCodes::BSONObjectTooLarge));
        // The error event carries the oversized event's own resume token, as both _id and sort key.
        ASSERT_VALUE_EQ(error->getField(DocumentSourceChangeStream::kIdField), token);
        ASSERT_VALUE_EQ(error->metadata().getSortKey(), token);
    }
}

TEST_F(ChangeStreamHelpersTest, OversizedEventErrorIsNoneWhenFlagIsOff) {
    // No showErrorsAsEvents: nothing is converted.
    const Value token(Document{{"_id", 1}});
    try {
        uasserted(ErrorCodes::BSONObjectTooLarge, "event too large");
    } catch (const DBException& ex) {
        ASSERT_FALSE(tryBuildOversizedEventError(ex, eventDoc(token), getExpCtx()).has_value());
    }
}

// An oversized event whose '_id' a user stage rewrote ({$addFields: {_id: {a: 1}}}) must not yield
// an error event carrying the user's value as its resume token: the stream fails closed with
// ChangeStreamFatalError, exactly as delivering the unmodified-size event would on master.
TEST_F(ChangeStreamHelpersTest, OversizedErrorFailsClosedWhenIdWasModified) {
    DocumentSourceChangeStreamSpec spec;
    spec.setShowErrorsAsEvents(true);
    getExpCtx()->setChangeStreamSpec(spec);

    const auto tokenData = requestedToken();
    const Value token(ResumeToken(tokenData).toDocument());
    for (const auto& userId : {Value(Document{{"a", 1}}), Value()}) {
        MutableDocument md;
        if (!userId.missing()) {
            md.addField(DocumentSourceChangeStream::kIdField, userId);
        }
        md.addField(DocumentSourceChangeStream::kClusterTimeField, Value(tokenData.clusterTime));
        md.metadata().setSortKey(token, true /* isSingleElementKey */);
        const auto event = md.freeze();

        try {
            uasserted(ErrorCodes::BSONObjectTooLarge, "event too large");
        } catch (const DBException& ex) {
            ASSERT_THROWS_CODE(tryBuildOversizedEventError(ex, event, getExpCtx()),
                               AssertionException,
                               ErrorCodes::ChangeStreamFatalError);
        }
    }
}

// An oversized error event is not re-wrapped into a BSONObjectTooLarge error event: that would
// replace the original error's code and message. Instead it fails closed with
// ChangeStreamFatalError, consistent with the path that builds a converted error event which is
// itself still too large.
TEST_F(ChangeStreamHelpersTest, OversizedErrorEventIsNotRewrapped) {
    DocumentSourceChangeStreamSpec spec;
    spec.setShowErrorsAsEvents(true);
    getExpCtx()->setChangeStreamSpec(spec);

    const Value token(Document{{"_id", 1}});
    MutableDocument md;
    md.addField(DocumentSourceChangeStream::kOperationTypeField,
                Value(DocumentSourceChangeStream::kErrorOpType));
    md.addField(DocumentSourceChangeStream::kIdField, token);
    md.metadata().setSortKey(token, true /* isSingleElementKey */);
    const auto errorEvent = md.freeze();

    try {
        uasserted(ErrorCodes::BSONObjectTooLarge, "event too large");
    } catch (const DBException& ex) {
        // An oversized error event is not re-wrapped: that would replace the original error's code
        // and message. It fails closed instead with ChangeStreamFatalError, consistent with the
        // path that builds a converted error event which is itself still too large.
        ASSERT_THROWS_CODE(tryBuildOversizedEventError(ex, errorEvent, getExpCtx()),
                           AssertionException,
                           ErrorCodes::ChangeStreamFatalError);
    }
}

// The oversized-event error conversion must build the error event from the pre-user-stage identity
// snapshot recorded on the ExpressionContext (as the transform does), not from the outgoing
// document 'doc'. A user $project/$addFields may have rewritten fields on 'doc' (e.g. documentKey);
// using 'doc' would leak those user-supplied values into the error event instead of the original
// event's fields. The identity is preferred when present; 'doc' is only a fallback when no snapshot
// is recorded (e.g. stages built in isolation in unit tests).
TEST_F(ChangeStreamHelpersTest, OversizedErrorUsesRecordedIdentityNotRewrittenDoc) {
    // The original identity the transform recorded for the in-flight event: pre-user-stage.
    const Value originalDocKey(Document{{"_id"sv, "orig"sv}});
    const Value token(Document{{"_id"sv, 42}});
    change_stream::recordActiveEventIdentity(
        getExpCtx(), eventDoc(token, Value(), Value(), Value(), originalDocKey));

    // The user's pipeline rewrote documentKey in the outgoing (oversized) event.
    DocumentSourceChangeStreamSpec spec;
    spec.setShowErrorsAsEvents(true);
    getExpCtx()->setChangeStreamSpec(spec);
    auto doc = eventDoc(token);
    MutableDocument rewritten(doc);
    rewritten[DocumentSourceChangeStream::kDocumentKeyField] =
        Value(Document{{"_id"sv, "user-rewritten"sv}});

    try {
        uasserted(ErrorCodes::BSONObjectTooLarge, "event too large");
    } catch (const DBException& ex) {
        auto error = tryBuildOversizedEventError(ex, rewritten.freeze(), getExpCtx());
        ASSERT_TRUE(error.has_value());
        // The error event carries the original documentKey from the recorded identity, not the
        // value the user's stage injected into the outgoing 'doc'.
        ASSERT_VALUE_EQ(error->getField(DocumentSourceChangeStream::kDocumentKeyField),
                        originalDocKey);
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

TEST_F(ChangeStreamHelpersTest, BackfillRewriteUsesStartAfterForTheResumeToken) {
    // The client's current resume position can be an invalidate notification, which is a valid
    // resume option only as 'startAfter'. The rewrite must therefore not turn it into
    // 'resumeAfter', which the server rejects for such a token.
    const auto nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    const BSONObj resumeToken = BSON("_data" << "token");
    const Timestamp backfillFrom(100, 1);

    AggregateCommandRequest request(
        nss, std::vector<BSONObj>{BSON("$changeStream" << BSON("startAfter" << resumeToken))});
    setBackfillingStartOnChangeStreamStage(request, backfillFrom, resumeToken);

    const auto stage = request.getPipeline()[0]["$changeStream"].Obj();
    ASSERT_FALSE(stage.hasField("resumeAfter"));
    ASSERT_TRUE(stage.hasField("startAfter"));
    ASSERT_BSONOBJ_EQ(resumeToken, stage["startAfter"].Obj());
    ASSERT_EQ(backfillFrom, stage["backfillingStartAtOperationTime"].timestamp());
}

TEST_F(ChangeStreamHelpersTest, BackfillRewriteUsesStartAfterForAResumeAfterClient) {
    // A client that opened with 'resumeAfter' (and therefore cannot be sitting on an invalidate
    // token) is rewritten the same way; 'startAfter' and 'resumeAfter' are equivalent for any
    // other token.
    const auto nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    const BSONObj resumeToken = BSON("_data" << "token");
    const Timestamp backfillFrom(100, 1);

    AggregateCommandRequest request(
        nss, std::vector<BSONObj>{BSON("$changeStream" << BSON("resumeAfter" << resumeToken))});
    setBackfillingStartOnChangeStreamStage(request, backfillFrom, resumeToken);

    const auto stage = request.getPipeline()[0]["$changeStream"].Obj();
    ASSERT_FALSE(stage.hasField("resumeAfter"));
    ASSERT_TRUE(stage.hasField("startAfter"));
    ASSERT_BSONOBJ_EQ(resumeToken, stage["startAfter"].Obj());
}

TEST_F(ChangeStreamHelpersTest, BackfillRewriteMakesAnImplicitStartPointExplicit) {
    // A brand-new stream has no explicit resume option; the rewrite must make its resolved start
    // point explicit so a retry does not re-resolve a later one.
    const auto nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    const Timestamp backfillFrom(100, 1);
    const Timestamp resolvedStartPoint(200, 5);

    AggregateCommandRequest request(nss, std::vector<BSONObj>{BSON("$changeStream" << BSONObj())});
    setBackfillingStartOnChangeStreamStage(
        request, backfillFrom, /*resumeToken=*/BSONObj(), resolvedStartPoint);

    const auto stage = request.getPipeline()[0]["$changeStream"].Obj();
    ASSERT_FALSE(stage.hasField("resumeAfter"));
    ASSERT_FALSE(stage.hasField("startAfter"));
    ASSERT_EQ(resolvedStartPoint, stage["startAtOperationTime"].timestamp());
    ASSERT_EQ(backfillFrom, stage["backfillingStartAtOperationTime"].timestamp());
}

TEST_F(ChangeStreamHelpersTest, BackfillRewriteAddsStartAfterWhenStageHasNoResumeOption) {
    // A stream opened without a resume option (a brand-new stream) can still be rebuilt with a
    // non-empty resume token, e.g. the current high-water mark on a getMore rebuild. The stage has
    // no resume option to replace, so the rewrite must append a 'startAfter' on its own.
    const auto nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    const BSONObj resumeToken = BSON("_data" << "token");
    const Timestamp backfillFrom(100, 1);

    AggregateCommandRequest request(
        nss, std::vector<BSONObj>{BSON("$changeStream" << BSON("fullDocument" << "updateLookup"))});
    setBackfillingStartOnChangeStreamStage(request, backfillFrom, resumeToken);

    ASSERT_BSONOBJ_EQ(
        BSON("$changeStream" << BSON("fullDocument" << "updateLookup" << "startAfter" << resumeToken
                                                    << "backfillingStartAtOperationTime"
                                                    << backfillFrom)),
        request.getPipeline()[0]);
}

TEST_F(ChangeStreamHelpersTest, BackfillRewriteReplacesExistingValuesOnRepeatedRebuilds) {
    const auto nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    const BSONObj originalToken = BSON("_data" << "original");
    const BSONObj downstreamStage = BSON("$match" << BSON("operationType" << "insert"));
    AggregateCommandRequest request(
        nss,
        std::vector<BSONObj>{
            BSON("$changeStream" << BSON("resumeAfter" << originalToken << "fullDocument"
                                                       << "updateLookup")),
            downstreamStage});

    for (int attempt = 1; attempt <= 2; ++attempt) {
        const BSONObj resumeToken = BSON("_data" << (attempt == 1 ? "first" : "second"));
        const Timestamp backfillFrom(100 + attempt, 1);
        setBackfillingStartOnChangeStreamStage(request, backfillFrom, resumeToken);

        ASSERT_EQ(2U, request.getPipeline().size());
        ASSERT_BSONOBJ_EQ(
            BSON("$changeStream" << BSON("fullDocument"
                                         << "updateLookup" << "startAfter" << resumeToken
                                         << "backfillingStartAtOperationTime" << backfillFrom)),
            request.getPipeline()[0]);
        ASSERT_BSONOBJ_EQ(downstreamStage, request.getPipeline()[1]);
    }
}

}  // namespace
}  // namespace mongo
