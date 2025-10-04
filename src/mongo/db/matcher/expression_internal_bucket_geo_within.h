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

#include "mongo/base/clonable_ptr.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/geo/geometry_container.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <s2cellid.h>

namespace mongo {

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
    static constexpr StringData kName = "$_internalBucketGeoWithin"_sd;
    static constexpr StringData kWithinRegion = "withinRegion"_sd;
    static constexpr StringData kField = "field"_sd;

    InternalBucketGeoWithinMatchExpression(std::shared_ptr<GeometryContainer> container,
                                           std::string field,
                                           clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : MatchExpression(MatchExpression::INTERNAL_BUCKET_GEO_WITHIN, std::move(annotation)),
          _geoContainer(container),
          _indexField("data." + field),
          _fieldRef(_indexField),
          _field(std::move(field)) {}

    void debugString(StringBuilder& debug, int indentationLevel) const final;

    bool equivalent(const MatchExpression* expr) const final;

    MatchCategory getCategory() const final {
        return MatchCategory::kLeaf;
    }

    void serialize(BSONObjBuilder* builder,
                   const SerializationOptions& opts = {},
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

    StringData path() const final {
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
};

}  // namespace mongo
