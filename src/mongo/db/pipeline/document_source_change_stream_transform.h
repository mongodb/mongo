/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/field_path.h"

namespace mongo {

class DocumentSourceChangeStreamTransform : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalChangeStreamTransform"_sd;
    /**
     * Creates a new transformation stage from the given specification.
     */
    static boost::intrusive_ptr<DocumentSourceChangeStreamTransform> create(
        const boost::intrusive_ptr<ExpressionContext>&,
        const ServerGlobalParams::FeatureCompatibility::Version&,
        BSONObj changeStreamSpec);

    Document applyTransformation(const Document& input);
    DepsTracker::State getDependencies(DepsTracker* deps) const final;
    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain) const;
    StageConstraints constraints(Pipeline::SplitState pipeState) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    const char* getSourceName() const {
        return DocumentSourceChangeStream::kStageName.rawData();
    }

protected:
    DocumentSource::GetNextResult doGetNext() override;

private:
    // This constructor is private, callers should use the 'create()' method above.
    DocumentSourceChangeStreamTransform(const boost::intrusive_ptr<ExpressionContext>&,
                                        const ServerGlobalParams::FeatureCompatibility::Version&,
                                        BSONObj changeStreamSpec);

    struct DocumentKeyCacheEntry {
        DocumentKeyCacheEntry() = default;

        DocumentKeyCacheEntry(std::pair<std::vector<FieldPath>, bool> documentKeyFieldsIn)
            : documentKeyFields(documentKeyFieldsIn.first), isFinal(documentKeyFieldsIn.second){};
        // Fields of the document key, in order, including "_id" and the shard key if the
        // collection is sharded. Empty until the first oplog entry with a uuid is encountered.
        // Needed for transforming 'insert' oplog entries.
        std::vector<FieldPath> documentKeyFields;

        // Set to true if the document key fields for this entry are definitively known and will
        // not change. This implies that either the collection has become sharded or has been
        // dropped.
        bool isFinal;
    };

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

        TransactionOpIterator(OperationContext* opCtx,
                              std::shared_ptr<MongoProcessInterface> mongoProcessInterface,
                              const Document& input,
                              const pcrecpp::RE& nsRegex);

        // Returns the index for the last operation returned by getNextTransactionOp(). It is
        // illegal to call this before calling getNextTransactionOp() at least once.
        size_t txnOpIndex() const {
            // 'txnOpIndex' points to the _next_ transaction index, so we must subtract one to get
            // the index of the entry being examined right now.
            invariant(_txnOpIndex >= 1);
            return _txnOpIndex - 1;
        }

        Timestamp clusterTime() const {
            return _clusterTime;
        }

        Document lsid() const {
            return _lsid;
        }

        TxnNumber txnNumber() const {
            return _txnNumber;
        }

        // Extract one Document from the transaction and advance the iterator. Returns boost::none
        // to indicate that there are no operations left.
        boost::optional<Document> getNextTransactionOp(OperationContext* opCtx);

    private:
        // Perform a find on the oplog to find an OplogEntry by its OpTime.
        repl::OplogEntry _lookUpOplogEntryByOpTime(OperationContext* opCtx,
                                                   repl::OpTime lookupTime) const;

        // Helper for getNextTransactionOp(). Checks the namespace of the given document to see if
        // it should be returned in the change stream.
        bool _isDocumentRelevant(const Document& d) const;

        // Traverse backwards through the oplog by starting at the entry at 'firstOpTime' and
        // following "prevOpTime" links until reaching the terminal "prevOpTime" value, and push the
        // OpTime value to '_txnOplogEntries' for each entry traversed, including the 'firstOpTime'
        // entry. Note that we follow the oplog links _backwards_ through the oplog (i.e., in
        // reverse chronological order) but because this is a stack, the iterator will process them
        // in the opposite order, allowing iteration to proceed fowards and return operations in
        // chronological order.
        void _collectAllOpTimesFromTransaction(OperationContext* opCtx, repl::OpTime firstOpTime);

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

        // Our current place within the entire transaction, which may consist of multiple 'applyOps'
        // arrays.
        size_t _txnOpIndex;

        // The clusterTime of the _applyOps.
        Timestamp _clusterTime;

        // Fields that were taken from the '_applyOps' oplog entry.
        Document _lsid;
        TxnNumber _txnNumber;

        // Used for traversing the oplog with TransactionHistoryInterface.
        std::shared_ptr<MongoProcessInterface> _mongoProcessInterface;

        // An operation is relevant to a change stream iff its namespace matches this regex.
        const pcrecpp::RE& _nsRegex;
    };

    /**
     * Helper used for determining what resume token to return.
     */
    ResumeTokenData getResumeToken(Value ts, Value uuid, Value documentKey);

    BSONObj _changeStreamSpec;

    // Map of collection UUID to document key fields.
    std::map<UUID, DocumentKeyCacheEntry> _documentKeyCache;

    // Regex for matching the "ns" field in applyOps sub-entries. Only used when we have a
    // change stream on the entire DB. When watching just a single collection, this field is
    // boost::none, and an exact string equality check is used instead.
    boost::optional<pcrecpp::RE> _nsRegex;

    // Represents the current transaction we're unwinding, if any.
    boost::optional<TransactionOpIterator> _txnIterator;

    // Set to true if this transformation stage can be run on the collectionless namespace.
    bool _isIndependentOfAnyCollection;

    // '_fcv' is used to determine which version of the resume token to generate for each change.
    // This is a snapshot of what the feature compatibility version was at the time the stream was
    // opened or resumed.
    ServerGlobalParams::FeatureCompatibility::Version _fcv;
};

}  // namespace mongo
