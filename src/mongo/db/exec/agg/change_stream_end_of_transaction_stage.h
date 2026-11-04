// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/agg/queue_stage.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <boost/functional/hash.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
class Pipeline;
}  // namespace mongo

namespace mongo::exec::agg {

/**
 * Identifies a transaction observed by the end-of-transaction synthesis stage: the logical session
 * id and transaction number carried by the shards' accounting control events.
 */
using EndOfTransactionTxnId = std::pair<LogicalSessionId, TxnNumber>;

/**
 * Hasher for 'EndOfTransactionTxnId'. 'LogicalSessionId' does not support 'absl::Hash', so the
 * default hasher cannot be used.
 */
struct EndOfTransactionTxnIdHash {
    std::size_t operator()(const EndOfTransactionTxnId& txnId) const {
        std::size_t hash = LogicalSessionIdHash{}(txnId.first);
        boost::hash_combine(hash, txnId.second);
        return hash;
    }
};

/**
 * The commit-time accounting metadata ('endOfTransactionScopes') carried by a shard's
 * end-of-transaction control event. It identifies the shard that produced the record and that
 * shard's view of the transaction's write participants, which the router uses to decide when the
 * transaction is complete for the watched scope.
 */
struct EndOfTransactionScopes {
    static EndOfTransactionScopes createFromDocument(const Document& scopes);

    bool operator==(const EndOfTransactionScopes& other) const = default;

    // The shard that produced the accounting record.
    ShardId shardId;
    // Map from collection namespace to the shards that wrote it.
    std::map<std::string, std::set<ShardId>> collectionParticipants;
    // Map from database name to the shards that wrote at least one namespace in it.
    std::map<std::string, std::set<ShardId>> databaseParticipants;
    // All shards that wrote transaction data.
    std::set<ShardId> clusterParticipants;
};

/**
 * A shard's end-of-transaction (EOT) accounting control event.
 *
 * Each write participant produces one such event; the router-side synthesis stage accumulates them
 * and turns them into the user-visible 'endOfTransaction' event once the transaction is complete
 * for the watched scope. This mirrors how topology control events are parsed into dedicated
 * structs, so the event's fields are validated and typed in one place instead of being read
 * ad-hoc from the raw control document.
 */
struct EndOfTransactionControlEvent {
    // Parses an accounting control event. The document must carry the accounting metadata
    // ('endOfTransactionScopes').
    static EndOfTransactionControlEvent createFromDocument(const Document& event);

    // Serializes the event into the user-visible 'endOfTransaction' change event document, using
    // 'resumeToken' as both the resume token ('_id') and the sort key. The internal accounting
    // metadata is not part of the user-visible event and is omitted.
    Document toDocument(const Value& resumeToken) const;

    bool operator==(const EndOfTransactionControlEvent& other) const;

    Timestamp clusterTime;
    Date_t wallTime;
    LogicalSessionId lsid;
    TxnNumber txnNumber;
    // The transaction's commit timestamp; present only for cross-shard transactions.
    boost::optional<Timestamp> commitTimestamp;
    // The accounting metadata identifying the writing shard and its participant view.
    EndOfTransactionScopes scopes;
};

/**
 * Accumulates the per-shard accounting control events observed by a single change stream, keyed by
 * (lsid, txnNumber), and decides when a transaction is complete for the stream's watched scope.
 *
 * This is the skeleton implementation: completion is purely set-based, driven by the participant
 * shard sets carried on the accounting control events. Shard availability, eviction and the
 * on-demand lookback are not implemented yet.
 */
class EndOfTransactionAggregator {
public:
    explicit EndOfTransactionAggregator(const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Records an accounting control event. Returns the completing event, which the caller
     * serializes into the user-visible 'endOfTransaction' event document, or boost::none if the
     * transaction is not yet complete for the watched scope.
     */
    boost::optional<EndOfTransactionControlEvent> tryCompleteTransaction(
        const EndOfTransactionControlEvent& controlEvent);

    /**
     * The number of transactions currently tracked by this aggregator.
     */
    size_t size() const {
        return _entries.size();
    }

private:
    struct PendingTransaction {
        // Expected participant shards for the watched scope, from the record's metadata.
        std::set<ShardId> expected;
        // Shard ids whose accounting record has been observed for this transaction.
        std::set<ShardId> seen;
        // The accounting control event with the largest commit-entry timestamp observed so far.
        EndOfTransactionControlEvent completingRecord;
    };

    // Returns the participant shards that the watched scope expects, from the accounting metadata.
    std::set<ShardId> _expectedShardsForWatchedScope(const EndOfTransactionScopes& scopes) const;

    // The watched change stream, used to resolve the stream's type and namespace.
    ChangeStream _changeStream;
    stdx::unordered_map<EndOfTransactionTxnId, PendingTransaction, EndOfTransactionTxnIdHash>
        _entries;
};

/**
 * Callback invoked with a synthesized event that survived the user stages, so the caller can
 * advance the results merger's client-visible post-batch resume token to the event's token.
 *
 * In production the callback forwards the event to the preceding topology-change stage, which owns
 * the results merger. Tests may replace it to observe the events that would be forwarded.
 */
using EndOfTransactionPbrtAdvancer = std::function<void(const Document&)>;

/**
 * An internal router-side stage used as part of the V2 change streams infrastructure to synthesize
 * user-visible 'endOfTransaction' events from the accounting control events produced by the shards.
 *
 * It is placed immediately after 'ChangeStreamHandleTopologyChangeV2' in the router part of a V2
 * change stream pipeline. Non-control events pass through unmodified. Topology control events must
 * have been consumed by the topology stage, so receiving one here is a programming error.
 * Accounting control events are accumulated per transaction and turned into a user-visible event
 * once the transaction completes for the watched scope.
 */
class ChangeStreamEndOfTransactionStage final : public Stage {
public:
    ChangeStreamEndOfTransactionStage(std::string_view stageName,
                                      const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                      std::shared_ptr<mongo::Pipeline> userPipeline);

    /**
     * Replaces the callback invoked with a synthesized event that survived the user stages, so
     * tests can observe the events that would be forwarded to the topology-change stage.
     */
    void setPbrtAdvancer_forTest(EndOfTransactionPbrtAdvancer advancer);

    /**
     * The number of transactions currently tracked by the aggregator.
     */
    size_t numTrackedTransactions_forTest() const {
        return _aggregator.size();
    }

private:
    GetNextResult doGetNext() final;

    void doDispose() final;

    void detachFromOperationContext() final;

    void reattachToOperationContext(OperationContext* opCtx) final;

    /**
     * Handles an accounting control event. Returns the synthesized user-visible event when the
     * transaction completes and the event survives the user stages, otherwise boost::none.
     */
    boost::optional<Document> _handleAccountingControlEvent(const Document& controlEvent);

    /**
     * Builds the executable form of '_userPipeline' into '_subExecPipeline'. A no-op when the
     * change stream has no user stages.
     */
    void _buildSubPipeline();

    /**
     * Runs the synthesized event through the user sub-pipeline, returning the transformed event, or
     * boost::none if the sub-pipeline filtered it out.
     */
    boost::optional<Document> _applyUserPipeline(const Document& event);

    /**
     * Forwards a synthesized event that survived the user stages to the preceding
     * 'ChangeStreamHandleTopologyChangeV2' stage, so that the results merger advances its
     * client-visible post-batch resume token to the event's token. This is the production
     * implementation of '_pbrtAdvancer'.
     */
    void _forwardPbrtToTopologyStage(const Document& event);

    EndOfTransactionAggregator _aggregator;

    // A copy of the user-defined single-document transformation stages that run on the shards, in
    // pipeline order. Null when the change stream has no such stages.
    std::shared_ptr<mongo::Pipeline> _userPipeline;

    // The executable form of '_userPipeline', built in the constructor. Null when the change stream
    // has no user stages.
    std::unique_ptr<exec::agg::Pipeline> _subExecPipeline;

    // The source stage of '_subExecPipeline', used to feed each synthesized event into it.
    exec::agg::QueueStage* _subExecQueue = nullptr;

    EndOfTransactionPbrtAdvancer _pbrtAdvancer;
};

}  // namespace mongo::exec::agg
