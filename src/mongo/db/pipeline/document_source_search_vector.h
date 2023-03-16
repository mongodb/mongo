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

namespace mongo {

class DocumentSourceSearchVector final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$searchVector"_sd;

    /**
     * Create a new $searchVector stage.
     */
    static boost::intrusive_ptr<DocumentSourceSearchVector> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        long long k,
        const std::string& lookupFieldName,
        const std::vector<double>& lookupVector);

    /**
     * Parse a $searchVector stage from a BSON stage specification.
     * 'elem's field name must be "$searchVector".
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kNone,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kAllowed,
                TransactionRequirement::kAllowed,
                LookupRequirement::kAllowed,
                UnionRequirement::kAllowed};
    }

    const char* getSourceName() const final {
        return kStageName.rawData();
    }

    /**
     * Attempts to combine with a subsequent $searchVector stage, setting 'searchVector' appropriately.
     */
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;
    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        return DepsTracker::State::SEE_NEXT;  // This doesn't affect needed fields
    }

    /**
     * Returns a DistributedPlanLogic with two identical $searchVector stages; one for the shards pipeline
     * and one for the merging pipeline.
     */
    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return DistributedPlanLogic{
            this, DocumentSourceSearchVector::create(pExpCtx, _k, _lookupFieldName, _lookupVector),
            boost::none};
    }

    unsigned int getK() const {
        return _k;
    }

    const std::string& getLookupFieldName() const {
        return _lookupFieldName;
    }

    const std::vector<double>& getLookupVector() const {
        return _lookupVector;
    }
    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

private:
    DocumentSourceSearchVector(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    long long k,
    const std::string& lookupFieldName,
    const std::vector<double>& lookupVector);

    GetNextResult doGetNext() final;
    const long long _k;
    const std::string& _lookupFieldName;
    const std::vector<double>& _lookupVector;
    long long _nReturned = 0;
};

}  // namespace mongo
