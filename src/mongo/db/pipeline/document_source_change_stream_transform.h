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

#include "mongo/db/pipeline/document_source_change_stream.h"

namespace mongo {

class DocumentSourceChangeStreamTransform : public DocumentSource,
                                            public ChangeStreamStageSerializationInterface {
public:
    static constexpr StringData kStageName = "$_internalChangeStreamTransform"_sd;

    /**
     * Creates a new transformation stage from the given specification.
     */
    static boost::intrusive_ptr<DocumentSourceChangeStreamTransform> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    Document applyTransformation(const Document& input);

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain) const final {
        return ChangeStreamStageSerializationInterface::serializeToValue(explain);
    }

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
     * Helper used for determining what resume token to return.
     */
    ResumeTokenData getResumeToken(Value ts, Value uuid, Value documentKey, Value txnOpIndex);

    Value serializeLegacy(boost::optional<ExplainOptions::Verbosity> explain) const final;
    Value serializeLatest(boost::optional<ExplainOptions::Verbosity> explain) const final;

    DocumentSourceChangeStreamSpec _changeStreamSpec;

    // Map of collection UUID to document key fields.
    std::map<UUID, DocumentKeyCacheEntry> _documentKeyCache;

    // Set to true if this transformation stage can be run on the collectionless namespace.
    bool _isIndependentOfAnyCollection;

    // Set to true if the pre-image optime should be included in output documents.
    bool _includePreImageOptime = false;
};

}  // namespace mongo
