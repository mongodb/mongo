// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/change_stream_end_of_transaction_stage.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/agg/change_stream_handle_topology_change_v2_stage.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_end_of_transaction.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamEndOfTransactionToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* endOfTransactionDS =
        dynamic_cast<DocumentSourceChangeStreamEndOfTransaction*>(documentSource.get());

    tassert(
        13766910, "expected 'DocumentSourceChangeStreamEndOfTransaction' type", endOfTransactionDS);

    return make_intrusive<exec::agg::ChangeStreamEndOfTransactionStage>(
        endOfTransactionDS->kStageName,
        endOfTransactionDS->getExpCtx(),
        endOfTransactionDS->getUserPipeline());
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(_internalChangeStreamEndOfTransaction,
                           DocumentSourceChangeStreamEndOfTransaction::id,
                           documentSourceChangeStreamEndOfTransactionToStageFn)

namespace {
using namespace std::literals::string_view_literals;

constexpr std::string_view kEndOfTransactionScopesField = "endOfTransactionScopes"sv;
constexpr std::string_view kShardIdField = "shardId"sv;
constexpr std::string_view kCollectionParticipantsField = "collectionParticipants"sv;
constexpr std::string_view kDatabaseParticipantsField = "databaseParticipants"sv;
constexpr std::string_view kClusterParticipantsField = "clusterParticipants"sv;

// Asserts that 'fieldName' of 'document' has the expected type and returns its value.
Value assertFieldType(const Document& document,
                      std::string_view fieldName,
                      BSONType expectedType,
                      int code) {
    const auto val = document[fieldName];
    uassert(code,
            str::stream() << "expected \"" << fieldName << "\" field to have type "
                          << typeName(expectedType) << ", instead found type "
                          << typeName(val.getType()),
            val.getType() == expectedType);
    return val;
}

// Parses an array of shard ids, ignoring any non-string entries.
std::set<ShardId> parseShardArray(const Value& value) {
    std::set<ShardId> shards;
    if (value.getType() != BSONType::array) {
        return shards;
    }
    for (const auto& shard : value.getArray()) {
        if (shard.getType() == BSONType::string) {
            shards.emplace(std::string(shard.getStringData()));
        }
    }
    return shards;
}

// Parses a map from name to an array of shard ids, ignoring any non-object entries.
std::map<std::string, std::set<ShardId>> parseParticipantMap(const Value& value) {
    std::map<std::string, std::set<ShardId>> participants;
    if (value.getType() != BSONType::object) {
        return participants;
    }
    const Document& document = value.getDocument();
    for (auto it = document.fieldIterator(); it.more();) {
        const auto [name, shards] = it.next();
        participants.emplace(std::string{name}, parseShardArray(shards));
    }
    return participants;
}
}  // namespace

EndOfTransactionAggregator::EndOfTransactionAggregator(
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : _changeStream(ChangeStream::buildFromExpressionContext(expCtx)) {}

EndOfTransactionScopes EndOfTransactionScopes::createFromDocument(const Document& scopes) {
    EndOfTransactionScopes result;
    result.shardId = assertFieldType(scopes, kShardIdField, BSONType::string, 13766901).getString();
    result.collectionParticipants = parseParticipantMap(scopes[kCollectionParticipantsField]);
    result.databaseParticipants = parseParticipantMap(scopes[kDatabaseParticipantsField]);
    result.clusterParticipants = parseShardArray(scopes[kClusterParticipantsField]);
    return result;
}

EndOfTransactionControlEvent EndOfTransactionControlEvent::createFromDocument(
    const Document& event) {
    const auto scopes = event[kEndOfTransactionScopesField];
    tassert(13766915,
            "accounting control event is missing its 'endOfTransactionScopes' metadata",
            scopes.getType() == BSONType::object);

    EndOfTransactionControlEvent result;
    result.clusterTime =
        assertFieldType(
            event, DocumentSourceChangeStream::kClusterTimeField, BSONType::timestamp, 13766912)
            .getTimestamp();
    result.wallTime =
        assertFieldType(event, DocumentSourceChangeStream::kWallTimeField, BSONType::date, 13766913)
            .getDate();
    result.lsid = LogicalSessionId::parse(
        assertFieldType(event, DocumentSourceChangeStream::kLsidField, BSONType::object, 13766904)
            .getDocument()
            .toBson(),
        IDLParserContext("EndOfTransactionControlEvent"));
    result.txnNumber =
        assertFieldType(
            event, DocumentSourceChangeStream::kTxnNumberField, BSONType::numberLong, 13766905)
            .getLong();
    if (const auto commitTimestamp = event[DocumentSourceChangeStream::kCommitTimestampField];
        commitTimestamp.getType() == BSONType::timestamp) {
        result.commitTimestamp = commitTimestamp.getTimestamp();
    }
    result.scopes = EndOfTransactionScopes::createFromDocument(scopes.getDocument());
    return result;
}

Document EndOfTransactionControlEvent::toDocument(const Value& resumeToken) const {
    MutableDocument document;
    document.addField(DocumentSourceChangeStream::kIdField, resumeToken);
    document.metadata().setSortKey(resumeToken, true);
    document.addField(DocumentSourceChangeStream::kOperationTypeField,
                      Value(DocumentSourceChangeStream::kEndOfTransactionOpType));
    document.addField(DocumentSourceChangeStream::kClusterTimeField, Value(clusterTime));
    document.addField(DocumentSourceChangeStream::kWallTimeField, Value(wallTime));
    if (commitTimestamp) {
        document.addField(DocumentSourceChangeStream::kCommitTimestampField,
                          Value(*commitTimestamp));
    }
    document.addField(DocumentSourceChangeStream::kLsidField, Value(lsid.toBSON()));
    document.addField(DocumentSourceChangeStream::kTxnNumberField,
                      Value(static_cast<long long>(txnNumber)));

    // The transaction identity is repeated inside 'operationDescription', which forms the event
    // identifier of the resume token and distinguishes events that share a cluster time.
    MutableDocument operationDescription;
    operationDescription.addField(DocumentSourceChangeStream::kLsidField, Value(lsid.toBSON()));
    operationDescription.addField(DocumentSourceChangeStream::kTxnNumberField,
                                  Value(static_cast<long long>(txnNumber)));
    document.addField(DocumentSourceChangeStream::kOperationDescriptionField,
                      Value(operationDescription.freeze()));

    return document.freeze();
}

bool EndOfTransactionControlEvent::operator==(const EndOfTransactionControlEvent& other) const {
    return clusterTime == other.clusterTime && wallTime == other.wallTime && lsid == other.lsid &&
        txnNumber == other.txnNumber && commitTimestamp == other.commitTimestamp &&
        scopes == other.scopes;
}

std::set<ShardId> EndOfTransactionAggregator::_expectedShardsForWatchedScope(
    const EndOfTransactionScopes& scopes) const {
    const boost::optional<NamespaceString> watchedNss = _changeStream.getNamespace();
    switch (_changeStream.getChangeStreamType()) {
        case ChangeStreamType::kCollection: {
            // Collection-scoped stream: the participants of the watched namespace.
            const NamespaceString& nss = *watchedNss;
            const std::string key = str::stream()
                << nss.dbName().db(omitTenant) << "." << nss.coll();
            const auto it = scopes.collectionParticipants.find(key);
            return it == scopes.collectionParticipants.end() ? std::set<ShardId>{} : it->second;
        }
        case ChangeStreamType::kDatabase: {
            // Database-scoped stream: the participants of the watched database.
            const std::string db{watchedNss->dbName().db(omitTenant)};
            const auto it = scopes.databaseParticipants.find(db);
            return it == scopes.databaseParticipants.end() ? std::set<ShardId>{} : it->second;
        }
        case ChangeStreamType::kAllDatabases:
            // Whole-cluster stream: every shard that wrote transaction data.
            return scopes.clusterParticipants;
    }
    MONGO_UNREACHABLE_TASSERT(13766911);
}

boost::optional<EndOfTransactionControlEvent> EndOfTransactionAggregator::tryCompleteTransaction(
    const EndOfTransactionControlEvent& controlEvent) {
    const EndOfTransactionTxnId key{controlEvent.lsid, controlEvent.txnNumber};
    auto& entry = _entries[key];
    entry.expected = _expectedShardsForWatchedScope(controlEvent.scopes);
    entry.seen.insert(controlEvent.scopes.shardId);
    // Accounting records are delivered in ascending (clusterTime, txnOpIndex) order, so the record
    // observed last carries the largest commit-entry timestamp.
    entry.completingRecord = controlEvent;

    // The transaction is complete for the watched scope once every expected participant has been
    // observed. Membership is set-based, so a re-observed record for the same shard is idempotent.
    if (entry.expected.empty() ||
        !std::includes(
            entry.seen.begin(), entry.seen.end(), entry.expected.begin(), entry.expected.end())) {
        return boost::none;
    }

    auto result = std::move(entry.completingRecord);
    _entries.erase(key);
    return result;
}

ChangeStreamEndOfTransactionStage::ChangeStreamEndOfTransactionStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    std::shared_ptr<mongo::Pipeline> userPipeline)
    : Stage(stageName, pExpCtx),
      _aggregator(pExpCtx),
      _userPipeline(std::move(userPipeline)),
      _pbrtAdvancer([this](const Document& event) { _forwardPbrtToTopologyStage(event); }) {
    _buildSubPipeline();
}

void ChangeStreamEndOfTransactionStage::setPbrtAdvancer_forTest(
    EndOfTransactionPbrtAdvancer advancer) {
    _pbrtAdvancer = std::move(advancer);
}

void ChangeStreamEndOfTransactionStage::_forwardPbrtToTopologyStage(const Document& event) {
    // The synthesis stage is placed directly after the topology-change stage in the router part of
    // a V2 change stream pipeline, so its source must be that stage, which owns the results merger
    // whose post-batch resume token is advanced here.
    auto* topologyStage = dynamic_cast<ChangeStreamHandleTopologyChangeV2Stage*>(pSource);
    tassert(13766907,
            "expected the end-of-transaction stage's source to be the topology-change stage",
            topologyStage);
    topologyStage->advancePostBatchResumeToken(event);
}

void ChangeStreamEndOfTransactionStage::doDispose() {
    if (_subExecPipeline) {
        _subExecPipeline->dispose();
        _subExecPipeline.reset();
    }
    _subExecQueue = nullptr;
    _userPipeline.reset();
}

void ChangeStreamEndOfTransactionStage::detachFromOperationContext() {
    if (_subExecPipeline) {
        _subExecPipeline->detachFromOperationContext();
    }
}

void ChangeStreamEndOfTransactionStage::reattachToOperationContext(OperationContext* opCtx) {
    if (_subExecPipeline) {
        _subExecPipeline->reattachToOperationContext(opCtx);
    }
}

void ChangeStreamEndOfTransactionStage::_buildSubPipeline() {
    if (!_userPipeline) {
        return;
    }

    // The extracted user stages have no source of their own. The queue stage provides one, letting
    // the same executable sub-pipeline be reused to feed each synthesized event through them.
    exec::agg::Pipeline::StageContainer stages;
    stages.push_back(make_intrusive<QueueStage>("$queue", pExpCtx, std::deque<GetNextResult>{}));
    for (const auto& userStage : _userPipeline->getSources()) {
        for (auto& stage : exec::agg::buildStages(userStage)) {
            exec::agg::stitchStage(*stage, stages.back().get());
            stages.push_back(stage);
        }
    }
    _subExecPipeline = std::make_unique<exec::agg::Pipeline>(std::move(stages), pExpCtx);
    _subExecQueue = static_cast<QueueStage*>(_subExecPipeline->getStages().front().get());
}

boost::optional<Document> ChangeStreamEndOfTransactionStage::_applyUserPipeline(
    const Document& event) {
    if (!_subExecPipeline) {
        return event;
    }

    _subExecQueue->emplace_back(GetNextResult(Document(event)));
    return _subExecPipeline->getNext();
}

GetNextResult ChangeStreamEndOfTransactionStage::doGetNext() {
    while (true) {
        GetNextResult next = pSource->getNext();

        if (!next.isAdvancedControlDocument()) {
            // Non-control results, including EOF and pause-execution, pass through unmodified.
            return next;
        }

        // Only "accounting" control events are expected here. Everything else is an error.
        const Document& controlEvent = next.getDocument();
        tassert(13766906,
                "change stream control event must be an accounting event",
                controlEvent.metadata().getChangeStreamControlEventType() ==
                    DocumentMetadataFields::ChangeStreamControlEventType::kAccounting);

        const auto opType = controlEvent[DocumentSourceChangeStream::kOperationTypeField];
        tassert(13766902,
                "change stream control event is missing a string 'operationType'",
                opType.getType() == BSONType::string);

        // An accounting control event must carry the 'endOfTransaction' operation type.
        tassert(13766903,
                str::stream() << "unexpected accounting control event operationType '"
                              << opType.getStringData() << "'; expected '"
                              << DocumentSourceChangeStream::kEndOfTransactionOpType << "'",
                opType.getStringData() == DocumentSourceChangeStream::kEndOfTransactionOpType);

        if (auto emitted = _handleAccountingControlEvent(controlEvent)) {
            return GetNextResult(std::move(*emitted));
        }

        // The accounting control event was swallowed; fetch the next input.
    }
}

boost::optional<Document> ChangeStreamEndOfTransactionStage::_handleAccountingControlEvent(
    const Document& controlEvent) {
    auto synthesized = _aggregator.tryCompleteTransaction(
        EndOfTransactionControlEvent::createFromDocument(controlEvent));
    if (!synthesized) {
        return boost::none;
    }

    // The synthesized event is a regular, user-visible change event: it carries neither the
    // change-stream control metadata nor the internal accounting metadata. Its resume token is the
    // completing record's token, which is also used as the sort key so the results merger can
    // advance the post-batch resume token to the event's token.
    Document event = synthesized->toDocument(controlEvent.metadata().getSortKey());

    // Re-apply the user pipeline stages. An event filtered out by the user stages is not emitted
    // and does not advance the post-batch resume token.
    auto userResult = _applyUserPipeline(event);
    if (!userResult) {
        return boost::none;
    }

    // The event survived, so advance the merger's client-visible post-batch resume token to the
    // emitted event's token.
    _pbrtAdvancer(*userResult);
    return userResult;
}

}  // namespace exec::agg
}  // namespace mongo
