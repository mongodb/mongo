// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/intrusive_ptr.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * Like DocumentSourceCursor, this stage returns Documents from BSONObjs produced by a PlanExecutor,
 * but does extra work to compute distances to satisfy a $near or $nearSphere query.
 */
class DocumentSourceGeoNearCursor final : public DocumentSourceCursor {
public:
    /**
     * The name of this stage.
     */
    static constexpr std::string_view kStageName = "$geoNearCursor"sv;

    /**
     * Create a new DocumentSourceGeoNearCursor. If specified, 'distanceMultiplier' must be
     * nonnegative.
     */
    static boost::intrusive_ptr<DocumentSourceGeoNearCursor> create(
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>,
        const boost::intrusive_ptr<ExpressionContext>&,
        boost::optional<FieldPath> distanceField,
        boost::optional<FieldPath> locationField = boost::none,
        double distanceMultiplier = 1.0);

    std::string_view getSourceName() const final;

    static const Id& id;

    Id getId() const final {
        return id;
    }

private:
    DocumentSourceGeoNearCursor(std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>,
                                const boost::intrusive_ptr<ExpressionContext>&,
                                boost::optional<FieldPath> distanceField,
                                boost::optional<FieldPath> locationField,
                                double distanceMultiplier);

    ~DocumentSourceGeoNearCursor() override = default;

    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceGeoNearCursorToStageFn(
        const boost::intrusive_ptr<DocumentSource>& source);

    // The output field in which to store the computed distance, if specified.
    boost::optional<FieldPath> _distanceField;
    // The output field to store the point that matched, if specified.
    boost::optional<FieldPath> _locationField;
    // A multiplicative factor applied to each distance. For example, you can use this to convert
    // radian distances into meters by multiplying by the radius of the Earth.
    double _distanceMultiplier = 1.0;
};
}  // namespace mongo
