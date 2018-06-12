/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_sources_gen.h"
#include "mongo/db/pipeline/field_path.h"

namespace mongo {

class DocumentSourceChangeStreamTransform : public DocumentSource {
public:
    /**
     * Creates a new transformation stage from the given specification.
     */
    static boost::intrusive_ptr<DocumentSourceChangeStreamTransform> create(
        const boost::intrusive_ptr<ExpressionContext>&, BSONObj changeStreamSpec);

    Document applyTransformation(const Document& input);
    DepsTracker::State getDependencies(DepsTracker* deps) const final;
    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain) const;
    DocumentSource::StageConstraints constraints(Pipeline::SplitState pipeState) const final;
    DocumentSource::GetNextResult getNext();
    const char* getSourceName() const {
        return DocumentSourceChangeStream::kStageName.rawData();
    }

private:
    // This constructor is private, callers should use the 'create()' method above.
    DocumentSourceChangeStreamTransform(const boost::intrusive_ptr<ExpressionContext>& expCtx,
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
     * Represents the DocumentSource's state if it's currently reading from an 'applyOps' entry
     * which was created as part of a transaction.
     */
    struct TransactionContext {
        MONGO_DISALLOW_COPYING(TransactionContext);

        // The array of oplog entries from an 'applyOps' representing the transaction. Only kept
        // around so that the underlying memory of 'arr' isn't freed.
        Value opArray;

        // Array representation of the 'opArray' field. Stored like this to avoid re-typechecking
        // each call to next(), or copying the entire array.
        const std::vector<Value>& arr;

        // Our current place in the 'opArray'.
        size_t pos;

        // The clusterTime of the applyOps.
        Timestamp clusterTime;

        // Fields that were taken from the 'applyOps' oplog entry.
        Document lsid;
        TxnNumber txnNumber;

        TransactionContext(const Value& applyOpsVal,
                           Timestamp ts,
                           const Document& lsidDoc,
                           TxnNumber n)
            : opArray(applyOpsVal),
              arr(opArray.getArray()),
              pos(0),
              clusterTime(ts),
              lsid(lsidDoc),
              txnNumber(n) {}
    };

    /**
     * Helper used for determining what resume token to return.
     */
    ResumeTokenData getResumeToken(Value ts, Value uuid, Value documentKey);
    void initializeTransactionContext(const Document& input);

    /**
     * Gets the next relevant applyOps entry that should be returned. If there is none, returns
     * empty document.
     */
    boost::optional<Document> extractNextApplyOpsEntry();

    /**
     * Helper for extractNextApplyOpsEntry(). Checks the namespace of the given document to see
     * if it should be returned in the change stream.
     */
    bool isDocumentRelevant(const Document& d);

    BSONObj _changeStreamSpec;

    // Map of collection UUID to document key fields.
    std::map<UUID, DocumentKeyCacheEntry> _documentKeyCache;

    // Regex for matching the "ns" field in applyOps sub-entries. Only used when we have a
    // change stream on the entire DB. When watching just a single collection, this field is
    // boost::none, and an exact string equality check is used instead.
    boost::optional<pcrecpp::RE> _nsRegex;

    // Represents if the current 'applyOps' we're unwinding, if any.
    boost::optional<TransactionContext> _txnContext;

    // Set to true if this transformation stage can be run on the collectionless namespace.
    bool _isIndependentOfAnyCollection;
};

}  // namespace mongo
