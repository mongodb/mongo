// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/geo/geometry_container.h"
#include "mongo/db/index/geo/s2_common.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <s2cellid.h>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * An internal $geoWithin Match expression that has the same semantics as $geoWithin but is only
 * used against timeseries collections.
 *
 * For example, consider the match expression
 *
 *  {$_internalBucketGeoWithin: {
 *       withinRegion: {
 *         $geometry: {
 *            type : "Polygon" ,
 *            coordinates: [ [ [ 0, 0 ], [ 3, 6 ], [ 6, 1 ], [ 0, 0 ] ] ]
 *         }
 *       },
 *       field: "location"
 *  }}
 *
 * The document to match should be a raw timeseries bucket document. Note that the 'field' value,
 * "location", does not reflect the path in the matching document but the path in the document in
 * the corresponding timeseries collection, because the document schema for raw timeseries buckets
 * is different.
 */
class InternalBucketGeoWithinMatchExpression final : public MatchExpression {
public:
    static constexpr std::string_view kName = "$_internalBucketGeoWithin"sv;
    static constexpr std::string_view kWithinRegion = "withinRegion"sv;
    static constexpr std::string_view kField = "field"sv;

    InternalBucketGeoWithinMatchExpression(
        std::shared_ptr<GeometryContainer> container,
        std::string field,
        clonable_ptr<ErrorAnnotation> annotation = nullptr,
        boost::optional<S2IndexVersion> indexVersion = boost::none)
        : MatchExpression(MatchExpression::INTERNAL_BUCKET_GEO_WITHIN, std::move(annotation)),
          _geoContainer(container),
          _indexField("data." + field),
          _fieldRef(_indexField),
          _field(std::move(field)),
          _indexVersion(indexVersion) {}

    void debugString(StringBuilder& debug, int indentationLevel) const final;

    bool equivalent(const MatchExpression* expr) const final;

    MatchCategory getCategory() const final {
        return MatchCategory::kLeaf;
    }

    void serialize(BSONObjBuilder* builder,
                   const query_shape::SerializationOptions& opts = {},
                   bool includePath = true) const final;

    std::unique_ptr<MatchExpression> clone() const final;

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }

    size_t numChildren() const final {
        return 0;
    }

    MatchExpression* getChild(size_t i) const final {
        MONGO_UNREACHABLE_TASSERT(6400208);
    }

    void resetChild(size_t, MatchExpression*) override {
        MONGO_UNREACHABLE;
    };

    std::string getField() const {
        return _field;
    }

    const GeometryContainer& getGeoContainer() const {
        return *_geoContainer;
    }

    boost::optional<S2IndexVersion> getIndexVersion() const {
        return _indexVersion;
    }

    std::string_view path() const final {
        return _indexField;
    }

    const FieldRef* fieldRef() const final {
        return &_fieldRef;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

private:
    std::shared_ptr<GeometryContainer> _geoContainer;
    std::string _indexField;
    FieldRef _fieldRef;
    std::string _field;
    boost::optional<S2IndexVersion> _indexVersion;
};

}  // namespace mongo
