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

#include "mongo/db/geo/geometry_container.h"
#include "mongo/db/pipeline/document_source.h"

namespace mongo {

/**
 * This is an internal stage that computes the distance between the given centroid and the value of
 * '_field' of the input Document.
 */
class DocumentSourceInternalGeoNearDistance final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalComputeGeoNearDistance"_sd;
    static constexpr StringData kNearFieldName = "near"_sd;
    static constexpr StringData kKeyFieldName = "key"_sd;
    static constexpr StringData kDistanceFieldFieldName = "distanceField"_sd;
    static constexpr StringData kDistanceMultiplierFieldName = "distanceMultiplier"_sd;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    DocumentSourceInternalGeoNearDistance(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                          std::string key,
                                          std::unique_ptr<PointWithCRS> centroid,
                                          const BSONObj& coords,
                                          std::string distanceField,
                                          double distanceMultiplier);

    const char* getSourceName() const override {
        return kStageName.rawData();
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const override {
        StageConstraints result = {
            StreamType::kStreaming,
            PositionRequirement::kNone,
            HostTypeRequirement::kNone,
            DiskUseRequirement::kNoDiskUse,
            FacetRequirement::kAllowed,
            TransactionRequirement::kAllowed,
            LookupRequirement::kAllowed,
            UnionRequirement::kAllowed,
        };
        result.canSwapWithMatch = true;
        return result;
    }

    DocumentSource::GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{_distanceField.fullPath()}, {}};
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        return boost::none;
    }

private:
    Value serialize(
        boost::optional<ExplainOptions::Verbosity> explain = boost::none) const override;

    GetNextResult doGetNext() override;

    std::string _key;
    std::unique_ptr<PointWithCRS> _centroid;
    BSONObj _coords;  // "near" option
    FieldPath _distanceField;
    double _distanceMultiplier;
};

}  // namespace mongo
