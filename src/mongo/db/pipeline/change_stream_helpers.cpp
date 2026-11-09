// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/change_stream_helpers.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/end_of_transaction_parameter.h"
#include "mongo/db/exec/document_value/value_comparator.h"
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

}  // namespace mongo::change_stream
