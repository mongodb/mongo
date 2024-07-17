/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#pragma once

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstddef>
#include <memory>
#include <set>
#include <stack>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * This stage keeps track of applyOps oplog entries that represent transactions and "unwinds" them
 * whenever an oplog entry commits a transaction. When the stage observes an applyOps or commit
 * command that commits a transaction, it emits one document for each operation in the transaction
 * that matches the namespace filter. The applyOps entries themselves are removed from the stage's
 * output, but all other entries pass through unmodified. Note that the namespace filter applies
 * only to unwound transaction operations, not to any other entries.
 */
class DocumentSourceChangeStreamUnwindTransaction : public DocumentSourceInternalChangeStreamStage {
public:
    static constexpr StringData kStageName = "$_internalChangeStreamUnwindTransaction"_sd;

    static boost::intrusive_ptr<DocumentSourceChangeStreamUnwindTransaction> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSourceChangeStreamUnwindTransaction> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    Value doSerialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    StageConstraints constraints(Pipeline::SplitState pipeState) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    const char* getSourceName() const override {
        return DocumentSourceChangeStreamUnwindTransaction::kStageName.rawData();
    }

protected:
    DocumentSource::GetNextResult doGetNext() override;

    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;

private:
    DocumentSourceChangeStreamUnwindTransaction(
        BSONObj filter, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Resets the transaction entry filter saved in the '_filter' and '_expression' fields.
     */
    void rebuild(BSONObj filter);

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

        TransactionOpIterator(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              const Document& input,
                              const MatchExpression* expression);

        /**
         * Returns the index for the last operation returned by getNextTransactionOp(). It is
         * illegal to call this before calling getNextTransactionOp() at least once.
         */
        size_t txnOpIndex() const {
            // 'txnOpIndex' points to the _next_ transaction index, so we must subtract one to get
            // the index of the entry being examined right now.
            invariant(_txnOpIndex >= 1);
            return _txnOpIndex - 1;
        }

        size_t applyOpsIndex() const {
            // 'currentApplyOpsIndex' points to the _next_ 'applyOps' index, so we must subtract one
            // to get the index of the entry being examined right now.
            invariant(_currentApplyOpsIndex >= 1);
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
        // Perform a find on the oplog to find an OplogEntry by its OpTime.
        repl::OplogEntry _lookUpOplogEntryByOpTime(OperationContext* opCtx,
                                                   repl::OpTime lookupTime) const;

        // Helper for getNextTransactionOp(). Performs assertions on the document inside
        // applyOps.
        void _assertExpectedTransactionEventFormat(const Document& d) const;

        // Traverse backwards through the oplog by starting at the entry at 'firstOpTime' and
        // following "prevOpTime" links until reaching the terminal "prevOpTime" value, and push the
        // OpTime value to '_txnOplogEntries' for each entry traversed, including the 'firstOpTime'
        // entry. Note that we follow the oplog links _backwards_ through the oplog (i.e., in
        // reverse chronological order) but because this is a stack, the iterator will process them
        // in the opposite order, allowing iteration to proceed fowards and return operations in
        // chronological order.
        void _collectAllOpTimesFromTransaction(OperationContext* opCtx, repl::OpTime firstOpTime);

        // Adds more transaction related information to the document containing unwinded
        // transaction.
        Document _addRequiredTransactionFields(const Document& doc) const;

        // For unprepared transactions, generates an artificial endOfTransaction oplog entry once
        // all events in the transaction have been exhausted.
        boost::optional<Document> _createEndOfTransactionIfNeeded();

        // Records all namespaces affected by the transaction. Used in generating an artificial
        // endOfTransaction oplog entry.
        void _addAffectedNamespaces(const Document& op);

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

        // Fields that were taken from the '_applyOps' oplog entry. They are optional because
        // they may not be present on applyOps generated by the BatchedWriteContext.
        boost::optional<Document> _lsid;
        boost::optional<TxnNumber> _txnNumber;

        // Used for traversing the oplog with TransactionHistoryInterface.
        std::shared_ptr<MongoProcessInterface> _mongoProcessInterface;

        // Only return entries matching this expression.
        const MatchExpression* _expression;
        std::unique_ptr<MatchExpression> _endOfTransactionExpression;

        // If lsid and txnNumber are present and the transaction is not prepared, we need to create
        // fake endOfTransaction oplog entry
        bool _needEndOfTransaction;

        // Set of affected namespaces. Only needed if we need endOfTransaction.
        absl::flat_hash_set<NamespaceString> _affectedNamespaces;

        // Set to true after iterator have returned endOfTransaction
        bool _endOfTransactionReturned = false;
    };

    // All transaction entries are filtered through this expression. This extra filtering step is
    // necessary because a transaction can contain a mix of operations from different namespaces. A
    // change stream needs to filter by namespace to ensure it does not return operations outside
    // the namespace it watches. As an optimization, this filter can also have predicates from
    // user-specified $match stages, allowing for early filtering of events that we know would be
    // filtered later in the pipeline.
    BSONObj _filter;
    std::unique_ptr<MatchExpression> _expression;

    // Represents the current transaction we're unwinding, if any.
    boost::optional<TransactionOpIterator> _txnIterator;
};

}  // namespace mongo
