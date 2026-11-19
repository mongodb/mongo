// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/change_stream_helpers.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/end_of_transaction_parameter.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <exception>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

Document makeChangeStreamErrorEvent(const DBException& ex,
                                    ChangeStreamErrorSource source,
                                    const Document& sourceEvent,
                                    boost::optional<ShardId> shardId) {
    // 'sourceEvent' is the failing change event in flight (the recorded identity of the error).
    const auto& metadata = sourceEvent.metadata();
    const Value token =
        metadata.hasSortKey() && metadata.isSingleElementKey() ? metadata.getSortKey() : Value();
    if (token.missing()) {
        // No describable identity for the failing event: the failure cannot be attributed to an
        // event, so preserve the active exception's type and let the stream fail closed. Reaching
        // the fallback with no exception in flight is a programming error.
        if (auto eptr = std::current_exception()) {
            std::rethrow_exception(eptr);
        }
        MONGO_UNREACHABLE_TASSERT(10562302);
    }

    const Value id = sourceEvent[DocumentSourceChangeStream::kIdField];
    uassert(ErrorCodes::ChangeStreamFatalError,
            str::stream() << "Encountered an event whose _id field, which contains the resume "
                             "token, was modified by the pipeline. Modifying the _id field of "
                             "an event makes it impossible to resume the stream from that "
                             "point. Only transformations that retain the unmodified _id field "
                             "are allowed. Expected: "
                          << BSON("_id" << token)
                          << " but found: " << (id.missing() ? BSONObj() : BSON("_id" << id)),
            ValueComparator::kInstance.evaluate(id == token));

    const std::string errorCodeName(ErrorCodes::errorString(ex.code()));

    LOGV2_DEBUG(10562301,
                3,
                "Change stream error event emitted",
                "source"_attr = toString(source),
                "errorCode"_attr = ex.code(),
                "errorCodeName"_attr = errorCodeName);

    const auto& status = ex.toStatus();
    MutableDocument errorDoc;
    errorDoc[DocumentSourceChangeStream::kErrorSourceField] = Value(toString(source));
    errorDoc[DocumentSourceChangeStream::kErrorCodeField] = Value(static_cast<int>(status.code()));
    errorDoc[DocumentSourceChangeStream::kErrorCodeNameField] = Value(errorCodeName);
    errorDoc[DocumentSourceChangeStream::kErrorMessageField] = Value(status.reason());
    if (shardId) {
        // The failure originated on a shard (e.g. a pre-/post-image or updateLookup lookup, or a
        // user stage running on the shard); name the shard, as resume-gap events do.
        errorDoc[DocumentSourceChangeStream::kShardIdField] = Value(shardId->toString());
    }

    MutableDocument event;
    event[DocumentSourceChangeStream::kOperationTypeField] =
        Value(DocumentSourceChangeStream::kErrorOpType);
    event[DocumentSourceChangeStream::kIdField] = token;
    event[DocumentSourceChangeStream::kErrorField] = Value(errorDoc.freeze());

    const Value clusterTime = sourceEvent[DocumentSourceChangeStream::kClusterTimeField];
    if (!clusterTime.missing()) {
        event[DocumentSourceChangeStream::kClusterTimeField] = clusterTime;
    }
    const Value wallTime = sourceEvent[DocumentSourceChangeStream::kWallTimeField];
    if (!wallTime.missing()) {
        event[DocumentSourceChangeStream::kWallTimeField] = wallTime;
    }
    const Value ns = sourceEvent[DocumentSourceChangeStream::kNamespaceField];
    if (!ns.missing() && ns.getType() == BSONType::object) {
        event[DocumentSourceChangeStream::kNamespaceField] = ns;
    }
    const Value documentKey = sourceEvent[DocumentSourceChangeStream::kDocumentKeyField];
    if (!documentKey.missing()) {
        event[DocumentSourceChangeStream::kDocumentKeyField] = documentKey;
    }

    constexpr bool isSingleElementKey = true;
    event.metadata().setSortKey(token, isSingleElementKey);
    return event.freeze();
}

}  // namespace mongo

namespace mongo::change_stream {

bool showEndOfTransactionRequested(const DocumentSourceChangeStreamSpec& spec) {
    return spec.getShowEndOfTransaction().value_or(ShowEndOfTransactionModeEnum::kOff) !=
        ShowEndOfTransactionModeEnum::kOff;
}

bool isEndOfTransactionRequired(const DocumentSourceChangeStreamSpec& spec) {
    return spec.getShowEndOfTransaction().value_or(ShowEndOfTransactionModeEnum::kOff) ==
        ShowEndOfTransactionModeEnum::kRequired;
}

EndOfTransactionBehavior determineEndOfTransactionBehavior(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const auto& spec = expCtx->getChangeStreamSpec();
    const bool newEndOfTransactionEnabled =
        feature_flags::gFeatureFlagEndOfTransactionChangeEventSynthesis.isEnabled() &&
        isAllowEndOfTransactionEnabled() && spec && showEndOfTransactionRequested(*spec);
    if (newEndOfTransactionEnabled) {
        return EndOfTransactionBehavior::kNew;
    }

    const bool legacyEndOfTransactionEnabled = spec && spec->getShowExpandedEvents() &&
        feature_flags::gFeatureFlagEndOfTransactionChangeEvent.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
    if (legacyEndOfTransactionEnabled) {
        return EndOfTransactionBehavior::kLegacy;
    }

    return EndOfTransactionBehavior::kNone;
}

bool isRouterOrNonShardedReplicaSet(const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (expCtx->getInRouter()) {
        return true;
    }
    if (expCtx->getForPerShardCursor()) {
        // Sharded cluster mongod.
        return false;
    }

    if (const auto* shardingState = ShardingState::get(expCtx->getOperationContext());
        shardingState) {
        auto role = shardingState->pollClusterRole();
        const bool isReplSet = !role.has_value();
        return isReplSet;
    } else {
        // Sharding state is not initialized. This is the case in unit tests and also on standalone
        // mongods. But on standalone mongods we do not support change streams, so we will never get
        // here on standalone mongods.
        return false;
    }
}

bool isShardLocalStream(const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (expCtx->getInRouter()) {
        return false;
    }
    if (expCtx->getFromRouter()) {
        // A '$_passthroughToShard' cursor forwarded by a router has a router-side merge that
        // consumes the accounting control events and enforces the 'required' guarantee, so it is
        // not shard-local. This must be checked before the per-shard-cursor test below, which would
        // otherwise also match a router-forwarded cursor.
        return false;
    }
    if (expCtx->getForPerShardCursor()) {
        // A user-requested '$_passthroughToShard' cursor running on a shard. Unlike a
        // router-forwarded cursor, it has no router-side merge.
        return true;
    }
    if (const auto* shardingState = ShardingState::get(expCtx->getOperationContext());
        shardingState) {
        // A direct-to-mongod stream on a shard mongod.
        return shardingState->pollClusterRole().has_value();
    }
    // Sharding state is not initialized. This is the case in unit tests and on standalone mongods.
    // Standalone mongods do not support change streams, so this is only reachable in tests; treat
    // it as a non-shard-local stream so that the replica-set / router behavior is the default.
    return false;
}

ResumeTokenData resolveResumeTokenFromSpec(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           const DocumentSourceChangeStreamSpec& spec) {

    if (spec.getStartAfter()) {
        return spec.getStartAfter()->getData();
    } else if (spec.getResumeAfter()) {
        return spec.getResumeAfter()->getData();
    } else if (spec.getStartAtOperationTime()) {
        return ResumeToken::makeHighWaterMarkToken(*spec.getStartAtOperationTime(),
                                                   expCtx->getChangeStreamTokenVersion())
            .getData();
    }
    tasserted(5666901,
              "Expected one of 'startAfter', 'resumeAfter' or 'startAtOperationTime' to be "
              "populated in $changeStream spec");
}

bool shouldEmitCollectionUUIDForChangeEvent(const DocumentSourceChangeStreamSpec& spec) {
    return spec.getShowExpandedEvents();
}

void recordActiveEventIdentity(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               const Document& doc) {
    // Retain only the identity fields needed to reconstruct an error event, rather than the whole
    // event. Building a self-contained snapshot (fresh BSON storage) rather than a reference to
    // 'doc' keeps the in-flight event's shared storage refcount at 1, so a downstream stage that
    // mutates the event does not pay for a copy-on-write clone of the entire document, and bounds
    // the memory retained per cursor. It also makes the recorded identity independent of later
    // $project/$addFields transformations that may strip these fields from the live event.
    MutableDocument identity;
    identity[DocumentSourceChangeStream::kIdField] = doc[DocumentSourceChangeStream::kIdField];
    identity[DocumentSourceChangeStream::kClusterTimeField] =
        doc[DocumentSourceChangeStream::kClusterTimeField];
    identity[DocumentSourceChangeStream::kWallTimeField] =
        doc[DocumentSourceChangeStream::kWallTimeField];
    identity[DocumentSourceChangeStream::kNamespaceField] =
        doc[DocumentSourceChangeStream::kNamespaceField];
    identity[DocumentSourceChangeStream::kDocumentKeyField] =
        doc[DocumentSourceChangeStream::kDocumentKeyField];

    // Preserve the resume-token sort key: it attributes a later failure to this event and is what a
    // client resumes from after the resulting error event is emitted.
    const auto& metadata = doc.metadata();
    if (metadata.hasSortKey() && metadata.isSingleElementKey()) {
        identity.metadata().setSortKey(metadata.getSortKey(), /* isSingleElementKey = */ true);
    }

    expCtx->setActiveChangeStreamEvent(identity.freeze());
}

boost::optional<Document> tryBuildErrorEvent(
    const DBException& ex, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (!changeStreamShowsErrorsAsEvents(expCtx)) {
        return boost::none;
    }
    const auto& activeEvent = expCtx->getActiveChangeStreamEvent();
    if (!activeEvent || !isConvertibleChangeStreamError(ex.code())) {
        return boost::none;
    }
    const auto source =
        expCtx->getActiveChangeStreamErrorSource().value_or(ChangeStreamErrorSource::kUserPipeline);
    return makeChangeStreamErrorEvent(
        ex,
        source,
        *activeEvent,
        expCtx->getMongoProcessInterface()->getShardId(expCtx->getOperationContext()));
}

boost::optional<Document> tryBuildOversizedEventError(
    const DBException& ex,
    const Document& doc,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (!changeStreamShowsErrorsAsEvents(expCtx)) {
        return boost::none;
    }
    // An error event must not be re-wrapped: converting it would replace the original error's
    // code and message with BSONObjectTooLarge. Fail the stream closed with ChangeStreamFatalError
    // (consistent with the sibling path below for a converted error event that is still too large)
    // rather than surfacing a misleading, retryable-looking BSONObjectTooLarge.
    const Value opType = doc[DocumentSourceChangeStream::kOperationTypeField];
    if (opType.getType() == BSONType::string &&
        opType.getStringData() == DocumentSourceChangeStream::kErrorOpType) {
        uasserted(ErrorCodes::ChangeStreamFatalError,
                  "the change stream error event exceeded the BSON size limit and must not be "
                  "re-wrapped");
    }
    // Prefer the pre-user-stage identity snapshot recorded by the transform: it carries the
    // original event fields (documentKey, clusterTime, ...), independent of any $project/$addFields
    // the user ran. Fall back to the outgoing 'doc' only when no snapshot is recorded (e.g. stages
    // built in isolation in unit tests).
    const auto& activeEvent = expCtx->getActiveChangeStreamEvent();
    const Document source = activeEvent ? *activeEvent : doc;
    return makeChangeStreamErrorEvent(
        ex,
        ChangeStreamErrorSource::kOversizedEvent,
        source,
        expCtx->getMongoProcessInterface()->getShardId(expCtx->getOperationContext()));
}

namespace {
// Counts change stream events that could not be delivered because they exceeded the BSON size
// limit, whether or not they were converted into error events. Registered at static-init time (the
// serverStatus metric tree is frozen shortly after startup) and bumped in exactly one place, so no
// serialization path can miss it. Lives here, not in the plan executor, because the top-level
// conversion stage also serializes events for opted-in streams.
auto& changeStreamsLargeEventsFailedCounter =
    *MetricBuilder<Counter64>{"changeStreams.largeEventsFailed"};

// Rebuilds a '$changeStream' stage for backfilling, replacing its resume options with the supplied
// 'resumeToken' (as 'startAfter') and setting 'backfillingStartAtOperationTime' to 'backfillFrom'.
// See 'setBackfillingStartOnChangeStreamStage' for the full contract.
BSONObj rewriteChangeStreamStageForBackfill(const BSONObj& stage,
                                            Timestamp backfillFrom,
                                            const BSONObj& resumeToken,
                                            boost::optional<Timestamp> resolvedStartPoint) {
    BSONObjBuilder builder;
    BSONObjBuilder changeStreamBuilder(builder.subobjStart(DocumentSourceChangeStream::kStageName));
    bool hasResumeOption = false;
    for (const auto& elem : stage.firstElement().Obj()) {
        const auto fieldName = elem.fieldNameStringData();
        if (fieldName ==
            DocumentSourceChangeStreamSpec::kBackfillingStartAtOperationTimeFieldName) {
            continue;
        }
        // Pipeline construction rejects a stage with more than one resume option, so at most one of
        // these is present. When a resume token is supplied, the existing option is dropped here
        // and a single 'startAfter' is appended below; the below append is also what covers a stage
        // that carries no resume option at all.
        if (fieldName == DocumentSourceChangeStreamSpec::kResumeAfterFieldName ||
            fieldName == DocumentSourceChangeStreamSpec::kStartAfterFieldName ||
            fieldName == DocumentSourceChangeStreamSpec::kStartAtOperationTimeFieldName) {
            hasResumeOption = true;
            if (!resumeToken.isEmpty()) {
                continue;
            }
        }
        changeStreamBuilder.append(elem);
    }
    if (!resumeToken.isEmpty()) {
        // Use 'startAfter' rather than 'resumeAfter': the client's current resume position may be
        // an invalidate notification (a stream opened with 'startAfter' an invalidate event that
        // has not advanced), which 'resumeAfter' rejects.
        changeStreamBuilder.append(DocumentSourceChangeStreamSpec::kStartAfterFieldName,
                                   resumeToken);
    } else if (!hasResumeOption && resolvedStartPoint) {
        changeStreamBuilder.append(DocumentSourceChangeStreamSpec::kStartAtOperationTimeFieldName,
                                   *resolvedStartPoint);
    }
    changeStreamBuilder.append(
        DocumentSourceChangeStreamSpec::kBackfillingStartAtOperationTimeFieldName, backfillFrom);
    changeStreamBuilder.doneFast();
    return builder.obj();
}
}  // namespace

void incrementLargeEventsFailedMetric() {
    changeStreamsLargeEventsFailedCounter.increment();
}

SerializedChangeStreamEvent serializeChangeStreamEventForExecutor(
    Document event, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const bool withMetadata = expCtx->getNeedsMerge() || expCtx->getForPerShardCursor();
    const auto serialize = [withMetadata](const Document& doc) {
        return withMetadata ? doc.toBsonWithMetaData() : doc.toBson();
    };

    BSONObj rawEvent;
    try {
        rawEvent = serialize(event);
    } catch (const ExceptionFor<ErrorCodes::BSONObjectTooLarge>& ex) {
        incrementLargeEventsFailedMetric();
        auto errorEvent = tryBuildOversizedEventError(ex, event, expCtx);
        if (!errorEvent) {
            throw;
        }
        event = std::move(*errorEvent);
        try {
            rawEvent = serialize(event);
        } catch (const ExceptionFor<ErrorCodes::BSONObjectTooLarge>&) {
            // The error event itself (e.g. a very large documentKey) still exceeds the BSON size
            // limit. There is nothing left to convert it into, so fail the stream closed rather
            // than leaking a BSONObjectTooLarge that implies a retryable oversized event.
            uasserted(ErrorCodes::ChangeStreamFatalError,
                      "the error event generated for an oversized change stream event still "
                      "exceeded the BSON size limit");
        }
    }

    // Move (not copy) the metadata out of 'event' so the caller can re-attach it cheaply without
    // carrying the whole event Document through the struct.
    MutableDocument md(std::move(event));
    return {std::move(rawEvent), md.releaseMetadata(), withMetadata};
}

Document makeExecutorReadyChangeStreamEvent(SerializedChangeStreamEvent serialized) {
    if (serialized.withMetadata) {
        // The BSON carries the metadata fields; re-parsing yields a Document that serializes back
        // with metadata in O(1).
        return Document::fromBsonWithMetaData(serialized.rawEvent);
    }

    // The BSON has no metadata fields, so re-attach the metadata (e.g. the resume-token sort key)
    // out-of-band. This keeps the document trivially convertible back to BSON while preserving
    // 'metadata()' for resume-token accounting and router validation.
    MutableDocument md;
    md.reset(serialized.rawEvent, /*bsonHasMetadata=*/false);
    md.setMetadata(std::move(serialized.metadata));
    return md.freeze();
}

repl::MutableOplogEntry createEndOfTransactionOplogEntry(
    const LogicalSessionId& lsid,
    const TxnNumber& txnNumber,
    const std::vector<NamespaceString>& affectedNamespaces,
    Timestamp timestamp,
    Date_t wallClock) {
    static const BSONObj kNoopEndOfTransactionMsgObject =
        BSON("msg" << BSON("endOfTransaction" << 1));

    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
    oplogEntry.setNss(NamespaceString::kAdminCommandNamespace);
    oplogEntry.setObject(kNoopEndOfTransactionMsgObject);

    oplogEntry.setSessionId(lsid);
    oplogEntry.setTxnNumber(txnNumber);

    BSONObjBuilder o2;
    {
        BSONArrayBuilder namespaces{o2.subarrayStart("endOfTransaction")};
        for (const auto& nss : affectedNamespaces) {
            namespaces.append(
                NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
        }
    }
    {
        BSONObjBuilder sessionId{o2.subobjStart(repl::OplogEntry::kSessionIdFieldName)};
        lsid.serialize(&sessionId);
    }
    o2.append(repl::OplogEntry::kTxnNumberFieldName, static_cast<long long>(txnNumber));
    oplogEntry.setObject2(o2.obj());

    oplogEntry.setTimestamp(timestamp);
    oplogEntry.setWallClockTime(wallClock);
    return oplogEntry;
}

Document makeEndOfTransactionChangeEvent(const Value& resumeToken,
                                         Timestamp clusterTime,
                                         Date_t wallTime,
                                         boost::optional<LogicalSessionId> lsid,
                                         boost::optional<TxnNumber> txnNumber,
                                         boost::optional<Timestamp> commitTimestamp,
                                         bool showExpandedEvents,
                                         bool showCommitTimestamp) {
    MutableDocument document;
    document.addField(DocumentSourceChangeStream::kIdField, resumeToken);
    document.metadata().setSortKey(resumeToken, true);
    document.addField(DocumentSourceChangeStream::kOperationTypeField,
                      Value(DocumentSourceChangeStream::kEndOfTransactionOpType));
    document.addField(DocumentSourceChangeStream::kClusterTimeField, Value(clusterTime));
    document.addField(DocumentSourceChangeStream::kWallTimeField, Value(wallTime));
    if (commitTimestamp && showCommitTimestamp) {
        document.addField(DocumentSourceChangeStream::kCommitTimestampField,
                          Value(*commitTimestamp));
    }

    // The transaction identity is omitted rather than asserted on when absent, so a malformed
    // 'o2.endOfTransaction' entry that drops it does not fail the stream with an invariant. The
    // router-side caller always supplies both.
    if (lsid) {
        document.addField(DocumentSourceChangeStream::kLsidField, Value(lsid->toBSON()));
    }
    if (txnNumber) {
        document.addField(DocumentSourceChangeStream::kTxnNumberField,
                          Value(static_cast<long long>(*txnNumber)));
    }

    // The transaction identity is repeated inside 'operationDescription', which forms the event
    // identifier of the resume token and distinguishes events that share a cluster time. Like
    // regular change events, the field is user-visible only when the stream was opened with
    // 'showExpandedEvents'.
    if (showExpandedEvents && lsid && txnNumber) {
        MutableDocument operationDescription;
        operationDescription.addField(DocumentSourceChangeStream::kLsidField,
                                      Value(lsid->toBSON()));
        operationDescription.addField(DocumentSourceChangeStream::kTxnNumberField,
                                      Value(static_cast<long long>(*txnNumber)));
        document.addField(DocumentSourceChangeStream::kOperationDescriptionField,
                          Value(operationDescription.freeze()));
    }

    return document.freeze();
}

void setBackfillingStartOnChangeStreamStage(AggregateCommandRequest& request,
                                            Timestamp backfillFrom,
                                            const BSONObj& resumeToken,
                                            boost::optional<Timestamp> resolvedStartPoint) {
    auto pipeline = request.getPipeline();
    bool found = false;
    for (auto& stage : pipeline) {
        if (stage.firstElementFieldNameStringData() != DocumentSourceChangeStream::kStageName) {
            continue;
        }

        stage = rewriteChangeStreamStageForBackfill(
            stage, backfillFrom, resumeToken, resolvedStartPoint);
        found = true;
        break;
    }

    tassert(13878204, "expected a $changeStream stage in the aggregate request", found);
    request.setPipeline(pipeline);
}

}  // namespace mongo::change_stream
