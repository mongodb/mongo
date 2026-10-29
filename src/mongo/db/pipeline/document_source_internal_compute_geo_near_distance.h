// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/geo/geometry_container.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>

#include <s2cellid.h>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(InternalComputeGeoNearDistance);

/**
 * This is an internal stage that computes the distance between the given centroid and the value of
 * '_field' of the input Document.
 */
class DocumentSourceInternalGeoNearDistance final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$_internalComputeGeoNearDistance"sv;
    static constexpr std::string_view kNearFieldName = "near"sv;
    static constexpr std::string_view kKeyFieldName = "key"sv;
    static constexpr std::string_view kDistanceFieldFieldName = "distanceField"sv;
    static constexpr std::string_view kDistanceMultiplierFieldName = "distanceMultiplier"sv;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    DocumentSourceInternalGeoNearDistance(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                          std::string key,
                                          std::unique_ptr<PointWithCRS> centroid,
                                          const BSONObj& coords,
                                          std::string distanceField,
                                          double distanceMultiplier);

    std::string_view getSourceName() const override {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const override {
        StageConstraints constraints = {
            StreamType::kStreaming,
            PositionRequirement::kNone,
            HostTypeRequirement::kNone,
            DiskUseRequirement::kNoDiskUse,
            FacetRequirement::kAllowed,
            TransactionRequirement::kAllowed,
            LookupRequirement::kAllowed,
            UnionRequirement::kAllowed,
        };
        constraints.preservesCardinality = true;
        constraints.canSwapWithMatch = true;
        constraints.outputDependsOnSingleInput = true;
        return constraints;
    }

    DocumentSource::GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{_distanceField.fullPath()}, {}};
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) override {
        return boost::none;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalGeoNearDistanceToStageFn(
        const boost::intrusive_ptr<DocumentSource>&);

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    std::string _key;
    std::unique_ptr<PointWithCRS> _centroid;
    BSONObj _coords;  // "near" option
    FieldPath _distanceField;
    double _distanceMultiplier;
};

}  // namespace mongo
