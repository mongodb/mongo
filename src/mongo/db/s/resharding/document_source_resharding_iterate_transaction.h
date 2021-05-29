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

#include "mongo/db/pipeline/document_source.h"

namespace mongo {

/**
 * This stage keeps track of applyOps oplog entries that represent transactions and iterates them
 * whenever an oplog entry commits a transaction. When the stage observes an applyOps or commit
 * command that commits a transaction, it emits one document for each applyOps in the transaction.
 *
 * Because it tracks whether or not a given oplog event is within a transaction, this stage is also
 * responsible for generating resharding's {clusterTime: <txn commit time or optime>, ts: <optime>}
 * _id field for *all* events, transaction or otherwise.
 */
class DocumentSourceReshardingIterateTransaction : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalReshardingIterateTransaction"_sd;

    static boost::intrusive_ptr<DocumentSourceReshardingIterateTransaction> create(
        const boost::intrusive_ptr<ExpressionContext>&);

    static boost::intrusive_ptr<DocumentSourceReshardingIterateTransaction> createFromBson(
        const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain) const;

    StageConstraints constraints(Pipeline::SplitState pipeState) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    const char* getSourceName() const {
        return DocumentSourceReshardingIterateTransaction::kStageName.rawData();
    }

protected:
    DocumentSource::GetNextResult doGetNext() override;

private:
    DocumentSourceReshardingIterateTransaction(
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Validates if the supplied document contains transaction details.
     */
    bool _isTransactionOplogEntry(const Document& doc);

    /**
     * Represents the DocumentSource's state if it's currently reading from a transaction.
     * Transaction operations are packed into 'applyOps' entries in the oplog.
     *
     * This iterator returns applyOps entries from a transaction in the same order they appear on
     * the oplog (chronological order). Note that the TransactionHistoryIterator, which this class
     * uses to query the oplog, returns the oplog entries in _reverse_ order. We internally reverse
     * the output of the TransactionHistoryIterator in order to get the desired order.
     */
    class TransactionOpIterator {
    public:
        TransactionOpIterator(const TransactionOpIterator&) = delete;
        TransactionOpIterator& operator=(const TransactionOpIterator&) = delete;

        TransactionOpIterator(OperationContext* opCtx,
                              std::shared_ptr<MongoProcessInterface> mongoProcessInterface,
                              const Document& input);

        Timestamp clusterTime() const {
            return _clusterTime;
        }

        Document lsid() const {
            return _lsid;
        }

        TxnNumber txnNumber() const {
            return _txnNumber;
        }

        /**
         * Returns the next 'applyOps' oplog entry from the transaction. Returns boost::none to
         * indicate that there are no operations left.
         */
        boost::optional<Document> getNextApplyOpsTxnEntry(OperationContext* opCtx);

    private:
        // Perform a find on the oplog to find an OplogEntry by its OpTime.
        repl::OplogEntry _lookUpOplogEntryByOpTime(OperationContext* opCtx,
                                                   repl::OpTime lookupTime) const;

        // Traverse backwards through the oplog by starting at the entry at 'firstOpTime' and
        // following "prevOpTime" links until reaching the terminal "prevOpTime" value, and push the
        // OpTime value to '_txnOplogEntries' for each entry traversed, including the 'firstOpTime'
        // entry. Note that we follow the oplog links _backwards_ through the oplog (i.e., in
        // reverse chronological order) but because this is a stack, the iterator will process them
        // in the opposite order, allowing iteration to proceed forwards and return operations in
        // chronological order.
        void _collectAllOpTimesFromTransaction(OperationContext* opCtx, repl::OpTime firstOpTime);

        // This stack contains the timestamps for all oplog entries in this transaction that have
        // yet to be processed by the iterator. Each time the TransactionOpIterator returns an
        // applyOps entry, it pops the next optime off the stack and uses it to load the next
        // applyOps entry, meaning that the top entry is always the next entry to be processed. From
        // top-to-bottom, the stack is ordered chronologically, in the same order as entries appear
        // in the oplog.
        std::stack<repl::OpTime> _txnOplogEntries;

        // The clusterTime of the entry which committed the transaction.
        Timestamp _clusterTime;

        // Fields that were taken from the oplog entry which committed the transaction.
        Document _lsid;
        TxnNumber _txnNumber;

        // Used for traversing the oplog with TransactionHistoryInterface.
        std::shared_ptr<MongoProcessInterface> _mongoProcessInterface;
    };

    // Represents the current transaction we're unwinding, if any.
    boost::optional<TransactionOpIterator> _txnIterator;
};

}  // namespace mongo
