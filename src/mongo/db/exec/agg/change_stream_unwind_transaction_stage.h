// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <stack>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

/**
 * How a change stream treats 'endOfTransaction' events. This is the resolved
 * 'showEndOfTransaction' mode: 'required' is downgraded to 'whenAvailable' for a shard-local
 * stream, which runs entirely on one shard and cannot count the other participants of a
 * multi-shard transaction.
 */
enum class EndOfTransactionMode {
    // The stream did not opt in via 'showEndOfTransaction'.
    kOff,
    // The stream opted in; an event is emitted whenever one is available.
    kWhenAvailable,
    // The stream opted in and requires an event; an ineligible transaction at the watched scope
    // fails the stream.
    kRequired,
};

/**
 * Stream-level endOfTransaction settings, resolved once when the stage is created and shared with
 * every TransactionOpIterator.
 */
struct EndOfTransactionOptions {
    // How the stream synthesizes endOfTransaction events, resolved from the feature flags and the
    // 'allowEndOfTransaction' server parameter.
    change_stream::EndOfTransactionBehavior behavior;
    // The stream's resolved 'showEndOfTransaction' mode.
    EndOfTransactionMode mode;
    // Whether the stream is router-bound (as opposed to a shard-local stream, which runs entirely
    // on the shard and has no router to consume control events). A user-requested
    // '$_passthroughToShard' cursor is forwarded to the shard with 'fromRouter: false' and is
    // shard-local, so this flag alone distinguishes it from a router-forwarded cursor.
    bool fromRouter;
};

/**
 * This class handles the execution part of the change stream unwind transaction aggregation stage
 * and is part of the execution pipeline. Its construction is based on
 * DocumentSourceChangeStreamUnwindTransaction, which handles the optimization part.
 */
class ChangeStreamUnwindTransactionStage final : public Stage {
public:
    ChangeStreamUnwindTransactionStage(std::string_view stageName,
                                       const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                       BSONObj filter,
                                       std::shared_ptr<MatchExpression> expression);

private:
    GetNextResult doGetNext() final;

    /**
     * Validates if the supplied document contains transaction details.
     */
    bool _isTransactionOplogEntry(const Document& doc);

    /**
     * Represents the DocumentSource's state if it's currently reading from a transaction.
     * Transaction operations are packed into 'applyOps' entries in the oplog.
     *
     * This iterator returns operations from a transaction that are relevant to the change stream in
     * the same order they appear on the oplog (chronological order). Note that the
     * TransactionHistoryIterator, which this class uses to query the oplog, returns the oplog
     * entries in _reverse_ order. We internally reverse the output of the
     * TransactionHistoryIterator in order to get the desired order.
     *
     * Note that our view of a transaction in the oplog is like an array of arrays with an "outer"
     * array of applyOps entries represented by the 'txnOplogEntries' field and "inner" arrays of
     * applyOps entries. Each applyOps entry gets loaded on demand, with only a single applyOps
     * loaded into '_applyOpsValue' and '_currentApplyOps' at any time.
     *
     * Likewise, there are "outer" and "inner" iterators, 'txnOplogEntriesIt' and
     * '_currentApplyOpsIt' respectively, that together reference the current transaction operation.
     */
    class TransactionOpIterator {
    public:
        TransactionOpIterator(const TransactionOpIterator&) = delete;
        TransactionOpIterator& operator=(const TransactionOpIterator&) = delete;

        TransactionOpIterator(const ChangeStreamUnwindTransactionStage* stage,
                              const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              const Document& input,
                              const MatchExpression* expression,
                              EndOfTransactionOptions endOfTransactionOptions);

        /**
         * Returns the index for the last operation returned by getNextTransactionOp(). It is
         * illegal to call this before calling getNextTransactionOp() at least once.
         */
        size_t txnOpIndex() const {
            // 'txnOpIndex' points to the _next_ transaction index, so we must subtract one to get
            // the index of the entry being examined right now.
            invariant(_txnOpIndex >= 1);
            invariant(_txnOpIndex - 1 <=
                      static_cast<size_t>(std::numeric_limits<long long>::max()));
            return _txnOpIndex - 1;
        }

        size_t applyOpsIndex() const {
            // 'currentApplyOpsIndex' points to the _next_ 'applyOps' index, so we must subtract one
            // to get the index of the entry being examined right now.
            invariant(_currentApplyOpsIndex >= 1);
            invariant(_currentApplyOpsIndex - 1 <=
                      static_cast<size_t>(std::numeric_limits<long long>::max()));
            return _currentApplyOpsIndex - 1;
        }

        /**
         * Returns the timestamp of the "applyOps" entry containing the last operation returned by
         * 'getNextTransactionOp()'. If 'getNextTransactionOp()' has not been called, returns the
         * timestamp of the first "applyOps" entry in the transaction.
         */
        Timestamp applyOpsTs() const {
            return _currentApplyOpsTs;
        }

        Timestamp clusterTime() const {
            return _clusterTime;
        }

        boost::optional<Document> lsid() const {
            return _lsid;
        }

        boost::optional<TxnNumber> txnNumber() const {
            return _txnNumber;
        }

        /**
         * Extract one Document from the transaction and advance the iterator. Returns boost::none
         * to indicate that there are no operations left.
         */
        boost::optional<Document> getNextTransactionOp(OperationContext* opCtx);

    private:
        /**
         * Throws ChangeStreamFatalError if the index of the current operation cannot be encoded.
         */
        void _assertTxnOpIndexIsEncodable() const;

        // Perform a find on the oplog to find an OplogEntry by its OpTime.
        repl::OplogEntry _lookUpOplogEntryByOpTime(OperationContext* opCtx,
                                                   repl::OpTime lookupTime) const;

        // Helper for getNextTransactionOp(). Performs assertions on the document inside
        // applyOps.
        void _assertExpectedTransactionEventFormat(const Document& doc) const;

        // Traverse backwards through the oplog by starting at the entry at 'firstOpTime' and
        // following "prevOpTime" links until reaching the terminal "prevOpTime" value, and push the
        // OpTime value to '_txnOplogEntries' for each entry traversed, including the 'firstOpTime'
        // entry. Note that we follow the oplog links _backwards_ through the oplog (i.e., in
        // reverse chronological order) but because this is a stack, the iterator will process them
        // in the opposite order, allowing iteration to proceed forwards and return operations in
        // chronological order.
        //
        // 'opsStillToCollect' bounds the walk for a retryable batch; an empty budget walks to the
        // end of the chain, as a transaction does. See walkApplyOpsChain().
        void _collectAllOpTimesFromTransaction(OperationContext* opCtx,
                                               repl::OpTime firstOpTime,
                                               boost::optional<std::size_t> opsStillToCollect);

        // Adds more transaction related information to the document containing unwinded
        // transaction.
        Document _addRequiredTransactionFields(const Document& doc) const;

        // For unprepared transactions, generates an artificial endOfTransaction oplog entry once
        // all events in the transaction have been exhausted.
        boost::optional<Document> _createEndOfTransactionIfNeeded();

        // Builds the artificial endOfTransaction event for the current transaction. Used both to
        // emit the event and to test whether an ineligible transaction touches the watched
        // namespace.
        Document _makeEndOfTransactionDocument();

        // Builds the internal accounting control event for a coordinated (multi-shard) transaction
        // from the commit entry's 'endOfTransactionScopes' metadata. The event carries the public
        // 'endOfTransaction' operation type and a resume token at the reserved
        // 'kEndOfTransactionTxnOpIndex', and is marked as an 'accounting' change stream control
        // event so that it is carried to the router unchanged.
        Document _makeEndOfTransactionControlEvent() const;

        // Records all namespaces affected by the transaction. Used in generating an artificial
        // endOfTransaction oplog entry.
        void _addAffectedNamespaces(const Document& op);

        // What the unwind stage must do with the current transaction once all of its operations
        // have been consumed. Exactly one outcome applies to a given transaction, so it is modeled
        // as a single value rather than as a set of interacting flags.
        enum class EndOfTransactionAction {
            // No endOfTransaction event is produced for this transaction.
            kNone,
            // Emit a synthesized, user-visible event if the transaction touched the watched
            // namespace. Used for unprepared and prepared replica-set / single-shard transactions.
            kEmitUserVisible,
            // Emit a synthesized, user-visible event. Used for a coordinated (multi-shard)
            // transaction on a shard-local stream, which has no router to synthesize the event from
            // an accounting control event.
            kEmitCoordinatedUserVisible,
            // Emit the internal accounting control event consumed by the router. Used for a
            // coordinated transaction on a router-bound stream whose accounting metadata
            // references the watched scope (see '_makeEndOfTransactionControlEvent()').
            kEmitAccountingControlEvent,
            // The stream requested 'showEndOfTransaction: required' and this transaction is
            // ineligible: fail the stream if the transaction touched the watched namespace,
            // otherwise produce nothing.
            kFailIfIneligibleTouchesWatchedNamespace,
        };

        // Returns whether the affected namespaces of the transaction must be tracked to carry out
        // 'action'.
        static bool _needsAffectedNamespaces(EndOfTransactionAction action);

        // This stack contains the timestamps for all oplog entries in this transaction that have
        // yet to be processed by the iterator. Each time the TransactionOpIterator finishes
        // iterating the contents of the '_currentApplyOps' array, it pops an entry off the stack
        // and uses it to load the next applyOps entry in the '_currentApplyOps' array, meaning that
        // the top entry is always the next entry to be processed. From top-to-bottom, the stack is
        // ordered chronologically, in the same order as entries appear in the oplog.
        std::stack<repl::OpTime> _txnOplogEntries;

        // The '_currentapplyOps' stores the applyOps array that the TransactionOpIterator is
        // currently iterating.
        Value _currentApplyOps;

        // This iterator references the next operation within the '_currentApplyOps' array that the
        // the getNextTransactionOp() method will return. When there are no more operations to
        // iterate, this iterator will point to the array's "end" sentinel, and '_txnOplogEntries'
        // will be empty.
        typename std::vector<Value>::const_iterator _currentApplyOpsIt;

        // The index of the next entry within the current 'applyOps' array.
        size_t _currentApplyOpsIndex;

        // The timestamp of the current 'applyOps' entry.
        Timestamp _currentApplyOpsTs;

        // Our current place within the entire transaction, which may consist of multiple 'applyOps'
        // arrays.
        size_t _txnOpIndex;

        // Cluster time and wall-clock time of the oplog entry which committed the transaction.
        Timestamp _clusterTime;
        Date_t _wallTime;

        // The "commitTimestamp" (the visible timestamp for the transaction) for prepared
        // transactions. Not set for non-prepared transactions.
        boost::optional<Timestamp> _commitTimestamp;

        // Fields that were taken from the '_applyOps' oplog entry. They are optional because
        // they may not be present on applyOps generated by the BatchedWriteContext.
        boost::optional<Document> _lsid;
        boost::optional<TxnNumber> _txnNumber;

        // Used for traversing the oplog with TransactionHistoryInterface.
        std::shared_ptr<MongoProcessInterface> _mongoProcessInterface;

        // Only return entries matching this expression.
        const MatchExpression* _expression;
        std::unique_ptr<MatchExpression> _endOfTransactionExpression;

        // What to do with the current transaction once its operations are exhausted. Cleared once
        // the action has been carried out, so that the event is produced at most once.
        EndOfTransactionAction _endOfTransactionAction = EndOfTransactionAction::kNone;

        // The raw 'endOfTransactionScopes' metadata of the current coordinated transaction, used to
        // build the accounting control event. Only set when '_endOfTransactionAction' is
        // 'kEmitAccountingControlEvent'.
        boost::optional<Document> _endOfTransactionScopes;

        // The change stream token version, used to build the accounting control event's resume
        // token.
        const int _changeStreamTokenVersion;

        // How this iterator synthesizes endOfTransaction events, resolved once when the stage was
        // created. A 'kNew' event uses the reserved 'kEndOfTransactionTxnOpIndex' rather than the
        // index after the last operation, so that the legacy path's tokens stay unchanged.
        const change_stream::EndOfTransactionBehavior _endOfTransactionBehavior;

        // Set of affected namespaces. Only needed if we need endOfTransaction.
        absl::flat_hash_set<NamespaceString> _affectedNamespaces;
    };

    // Returns true if this shard is a write participant of the scope watched by this change stream,
    // according to the 'endOfTransactionScopes' accounting metadata of a coordinated transaction.
    // A whole-cluster stream watches every scope, so every write participant qualifies.
    bool _endOfTransactionScopesCoverWatchedScope(const Document& scopes) const;

    // Represents the current transaction we're unwinding, if any.
    boost::optional<TransactionOpIterator> _txnIterator;

    // Resolved once when the stage is created; shared with every TransactionOpIterator.
    const EndOfTransactionOptions _endOfTransactionOptions;

    // The change stream watched by this stage, built once. Used to decide whether a coordinated
    // transaction's accounting metadata covers the watched scope.
    const ChangeStream _changeStream;

    // All transaction entries are filtered through this expression. This extra filtering step is
    // necessary because a transaction can contain a mix of operations from different namespaces. A
    // change stream needs to filter by namespace to ensure it does not return operations outside
    // the namespace it watches. As an optimization, this filter can also have predicates from
    // user-specified $match stages, allowing for early filtering of events that we know would be
    // filtered later in the pipeline.
    BSONObj _filter;
    std::shared_ptr<MatchExpression> _expression;
};
}  // namespace mongo::exec::agg
