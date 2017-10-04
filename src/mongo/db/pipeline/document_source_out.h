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

namespace mongo {

class DocumentSourceOut final : public DocumentSourceNeedsMongoProcessInterface,
                                public SplittableDocumentSource {
public:
    static std::unique_ptr<LiteParsedDocumentSourceForeignCollections> liteParse(
        const AggregationRequest& request, const BSONElement& spec);

    // virtuals from DocumentSource
    ~DocumentSourceOut() final;
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;
    GetDepsReturn getDependencies(DepsTracker* deps) const final;

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kLast,
                HostTypeRequirement::kPrimaryShard,
                DiskUseRequirement::kWritesPersistentData,
                FacetRequirement::kNotAllowed};
    }

    // Virtuals for SplittableDocumentSource
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
      Create a document source for output and pass-through.

      This can be put anywhere in a pipeline and will store content as
      well as pass it on.

      @param pBsonElement the raw BSON specification for the source
      @param pExpCtx the expression context for the pipeline
      @returns the newly created document source
    */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    DocumentSourceOut(const NamespaceString& outputNs,
                      const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Sets '_tempNs' to a unique temporary namespace, makes sure the output collection isn't
     * sharded or capped, and saves the collection options and indexes of the target collection.
     * Then creates the temporary collection we will insert into by copying the collection options
     * and indexes from the target collection.
     *
     * Sets '_initialized' to true upon completion.
     */
    void initialize();

    /**
     * Inserts all of 'toInsert' into the temporary collection.
     */
    void spill(const std::vector<BSONObj>& toInsert);

    bool _initialized = false;
    bool _done = false;

    // Holds on to the original collection options and index specs so we can check they didn't
    // change during computation.
    BSONObj _originalOutOptions;
    std::list<BSONObj> _originalIndexes;

    NamespaceString _tempNs;          // output goes here as it is being processed.
    const NamespaceString _outputNs;  // output will go here after all data is processed.
};

}  // namespace mongo
