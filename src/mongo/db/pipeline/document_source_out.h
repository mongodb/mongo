/**
 * Copyright (C) 2016 MongoDB Inc.
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
#include "mongo/db/pipeline/document_source_out_gen.h"

namespace mongo {

/**
 * Abstract class for the $out aggregation stage.
 */
class DocumentSourceOut : public DocumentSource, public NeedsMergerDocumentSource {
public:
    static std::unique_ptr<LiteParsedDocumentSourceForeignCollections> liteParse(
        const AggregationRequest& request, const BSONElement& spec);

    DocumentSourceOut(const NamespaceString& outputNs,
                      const boost::intrusive_ptr<ExpressionContext>& expCtx,
                      WriteModeEnum mode,
                      boost::optional<Document> uniqueKey);

    virtual ~DocumentSourceOut() = default;

    GetNextResult getNext() final;
    const char* getSourceName() const final;
    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;
    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kLast,
                HostTypeRequirement::kPrimaryShard,
                DiskUseRequirement::kWritesPersistentData,
                FacetRequirement::kNotAllowed,
                TransactionRequirement::kNotAllowed};
    }

    // Virtuals for NeedsMergerDocumentSource
    boost::intrusive_ptr<DocumentSource> getShardSource() final {
        return NULL;
    }
    std::list<boost::intrusive_ptr<DocumentSource>> getMergeSources() final {
        return {this};
    }

    const NamespaceString& getOutputNs() const {
        return _outputNs;
    }

    /**
     * Retrieves the namespace to direct each batch to, which may be a temporary namespace or the
     * final output namespace.
     */
    virtual const NamespaceString& getWriteNs() const = 0;

    /**
     * Prepares the DocumentSource to be able to write incoming batches to the desired collection.
     */
    virtual void initializeWriteNs() = 0;

    /**
     * Finalize the output collection, called when there are no more documents to write.
     */
    virtual void finalize() = 0;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    /**
     * Inserts all of 'toInsert' into the collection returned from getWriteNs().
     */
    void spill(const std::vector<BSONObj>& toInsert);

    bool _initialized = false;
    bool _done = false;

    const NamespaceString _outputNs;
    WriteModeEnum _mode;
    boost::optional<Document> _uniqueKey;
};

}  // namespace mongo
