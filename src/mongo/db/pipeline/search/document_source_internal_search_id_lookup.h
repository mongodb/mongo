/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/document_source.h"

namespace mongo {

/**
 * Queries local collection for _id equality matches. Intended for use with
 * $_internalSearchMongotRemote (see $search) as part of the Search project.
 *
 * Input documents will be ignored and skipped if they do not have a value at field "_id".
 * Input documents will be ignored and skipped if no document with key specified at "_id"
 * is locally-stored.
 */
class DocumentSourceInternalSearchIdLookUp final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalSearchIdLookup"_sd;
    /**
     * Creates an $_internalSearchIdLookup stage. "elem" must be an empty object.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    DocumentSourceInternalSearchIdLookUp(const boost::intrusive_ptr<ExpressionContext>& pExpCtx);
    DocumentSourceInternalSearchIdLookUp(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                         long long limit);

    const char* getSourceName() const final;

    DocumentSourceType getType() const override {
        return DocumentSourceType::kInternalSearchIdLookUp;
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const override {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kAnyShard,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed,
                                     ChangeStreamRequirement::kDenylist);
        // Set to true to allow this to be run on the shards before the search implicit sort.
        constraints.preservesOrderAndMetadata = true;

        return constraints;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        // This just depends on the '_id' field.
        deps->fields.insert("_id");
        return DepsTracker::State::SEE_NEXT;
    }
    /**
     * Serialize this stage - return is of the form { $_internalSearchIdLookup: {} }
     */
    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    /**
     * This stage must be run on each shard.
     */
    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        DistributedPlanLogic logic;

        logic.shardsStage = this;

        return logic;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    /**
     * This is state that is to be shared between the DocumentInternalSearchMongotRemote and
     * DocumentInternalSearchIdLookup stages (these stages are the result of desugaring $search)
     * during runtime.
     */
    class SearchIdLookupMetrics {
    public:
        SearchIdLookupMetrics() {}

        long long getDocsReturnedByIdLookup() const {
            return _docsReturnedByIdLookup;
        }

        void incrementDocsSeenByIdLookup() {
            _docsSeenByIdLookup++;
        }

        void incrementDocsReturnedByIdLookup() {
            _docsReturnedByIdLookup++;
        }

        /**
         * Sets the value of _docsSeenByIdLookup & _docsReturnedByLookup to 0.
         */
        void resetIdLookupMetrics() {
            _docsSeenByIdLookup = 0;
            _docsReturnedByIdLookup = 0;
        }

        /**
         * Returns the "success rate" of finding docs by id in the idLookup phase as
         * a floating point number between 0 and 1, where 0 is 0% and 1 is 100%.
         * For example, if idLookup has seen 6 documents and 3 were found,
         * this function would return 0.5 = 50%.
         */
        double getIdLookupSuccessRate() const {
            // Ensure division by zero never occurs if no docs have been seen yet
            if (_docsSeenByIdLookup == 0) {
                return 0;
            }

            tassert(9074400,
                    str::stream() << "_docsReturnedByIdLookup must not be greater than "
                                  << "_docsSeenByIdLookup in SearchIdLookupMetrics, but "
                                     "_docsReturnedByIdLookup = '"
                                  << _docsReturnedByIdLookup << "' and _docsSeenByIdLookup = '"
                                  << _docsSeenByIdLookup << "'.",
                    !(_docsSeenByIdLookup < _docsReturnedByIdLookup));

            return double(_docsReturnedByIdLookup) / double(_docsSeenByIdLookup);
        }

    private:
        // Number of documents that have been passed through the idLookup phase
        // (regardless of whether they were found or not).
        long long _docsSeenByIdLookup = 0;

        // When there is an extractable limit in the query, DocumentInternalSearchMongotRemote sends
        // a getMore to mongot that specifies how many more documents it needs to fulfill that
        // limit, and it incorporates the amount of documents returned by the
        // DocumentInternalSearchIdLookup stage into that value.
        long long _docsReturnedByIdLookup = 0;
    };

    std::shared_ptr<SearchIdLookupMetrics> getSearchIdLookupMetrics() {
        return _searchIdLookupMetrics;
    }

    const SpecificStats* getSpecificStats() const override {
        return &_stats;
    }

protected:
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) override;

private:
    DocumentSource::GetNextResult doGetNext() final;

    long long _limit = 0;

    std::shared_ptr<SearchIdLookupMetrics> _searchIdLookupMetrics =
        std::make_shared<SearchIdLookupMetrics>();

    DocumentSourceIdLookupStats _stats;
};

}  // namespace mongo
