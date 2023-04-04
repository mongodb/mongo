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


#include <boost/optional.hpp>
#include <s2cellid.h>
#include <s2cellunion.h>
#include <s2regioncoverer.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/timeseries/timeseries_constants.h"


namespace mongo {
constexpr StringData InternalBucketGeoWithinMatchExpression::kName;

void InternalBucketGeoWithinMatchExpression::debugString(StringBuilder& debug,
                                                         int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);

    BSONObjBuilder builder;
    serialize(&builder, {});
    debug << builder.obj().toString();
    _debugStringAttachTagInfo(&debug);
}

bool InternalBucketGeoWithinMatchExpression::equivalent(const MatchExpression* expr) const {
    if (matchType() != expr->matchType()) {
        return false;
    }

    const auto* other = static_cast<const InternalBucketGeoWithinMatchExpression*>(expr);

    return SimpleBSONObjComparator::kInstance.evaluate(
               _geoContainer->getGeoElement().Obj() ==
               other->getGeoContainer().getGeoElement().Obj()) &&
        _field == other->getField();
}

bool InternalBucketGeoWithinMatchExpression::matches(const MatchableDocument* doc,
                                                     MatchDetails* details) const {
    return _matchesBSONObj(doc->toBSON());
}

namespace {

/**
 * Dereference a path, treating each component of the path as an object field-name.
 *
 * If we try to traverse through an array or scalar, return boost::none to indicate that
 * dereferencing the path failed.
 *
 * However, traversing through missing succeeds, returning missing.
 * If this function returns missing, then the path only traversed through objects and missing.
 */
boost::optional<BSONElement> derefPath(const BSONObj& obj, const FieldPath& fieldPath) {
    // Dereferencing the first path component always succeeds, because we start with an object,
    // and because paths always have at least one component.
    auto elem = obj[fieldPath.front()];
    // Then we have zero or more path components.
    for (size_t i = 1; i < fieldPath.getPathLength(); ++i) {
        if (elem.eoo())
            return BSONElement{};
        if (elem.type() != BSONType::Object)
            return boost::none;
        elem = elem.Obj()[fieldPath.getFieldName(i)];
    }
    return elem;
}

}  // namespace

bool InternalBucketGeoWithinMatchExpression::_matchesBSONObj(const BSONObj& obj) const {
    // Look up the path in control.min and control.max.
    // If it goes through an array, return true to avoid dealing with implicit array traversal.
    // If it goes through a scalar, return true to avoid dealing with mixed types.
    auto controlMinElmOptional = derefPath(obj, timeseries::kControlMinFieldNamePrefix + _field);
    if (!controlMinElmOptional)
        return true;

    auto controlMaxElmOptional = derefPath(obj, timeseries::kControlMaxFieldNamePrefix + _field);
    if (!controlMaxElmOptional)
        return true;

    auto controlMinElm = *controlMinElmOptional;
    auto controlMaxElm = *controlMaxElmOptional;

    if (controlMinElm.eoo() && controlMaxElm.eoo()) {
        // If both min and max are missing, and we got here only traversing through objects and
        // missing, then this path is missing on every event in the bucket.
        return false;
    }
    if (controlMinElm.eoo() || controlMaxElm.eoo() || !controlMinElm.isABSONObj() ||
        !controlMaxElm.isABSONObj()) {
        // If either min or max is missing or a scalar, we may have mixed types, so conservatively
        // return true.
        return true;
    }

    if (controlMinElm.type() != controlMaxElm.type())
        return true;

    // Returns true if the bucket should be unpacked and all the data within the bucket should be
    // checked later.
    auto parseMinMaxPoint = [](const BSONElement& elem, PointWithCRS* out) -> bool {
        auto geoObj = elem.embeddedObject();
        if (Array == elem.type() || geoObj.firstElement().isNumber()) {
            // Legacy Point.
            if (!GeoParser::parseLegacyPoint(elem, out, true).isOK())
                return true;
        } else {
            // If the bucket contains GeoJSON objects of types other than 'Points' we cannot be sure
            // whether this bucket contains data is within the provided region.
            if (!geoObj.hasField(GEOJSON_TYPE) ||
                geoObj[GEOJSON_TYPE].String() != GEOJSON_TYPE_POINT)
                return true;
            // GeoJSON Point.
            if (!GeoParser::parseGeoJSONPoint(geoObj, out).isOK())
                return true;
        }
        return false;
    };

    PointWithCRS minPoint;
    PointWithCRS maxPoint;
    const CRS crs = _geoContainer->getNativeCRS();

    if (parseMinMaxPoint(controlMinElm, &minPoint) || parseMinMaxPoint(controlMaxElm, &maxPoint))
        return true;
    if (minPoint.crs != maxPoint.crs || crs != minPoint.crs)
        return true;

    if (crs == FLAT && _geoContainer->hasR2Region()) {
        // Construct a 2D Box for legacy coordinate pairs. This box is used to check if the box may
        // contain points that are within _geoContainer.
        const Box bbox2D(minPoint.oldPoint, maxPoint.oldPoint);
        if (_geoContainer->getR2Region().fastDisjoint(bbox2D))
            return false;
    }

    if (crs == SPHERE && _geoContainer->hasS2Region()) {
        const S2LatLngRect rect(S2LatLng(minPoint.point), S2LatLng(maxPoint.point));

        S2RegionCoverer coverer;
        S2CellUnion cellUnionRect;
        S2CellUnion cellUnionGeo;
        coverer.GetCellUnion(rect, &cellUnionRect);
        coverer.GetCellUnion(_geoContainer->getS2Region(), &cellUnionGeo);

        if (!cellUnionRect.Intersects(&cellUnionGeo))
            return false;
    }

    return true;
}

void InternalBucketGeoWithinMatchExpression::serialize(BSONObjBuilder* builder,
                                                       SerializationOptions opts) const {
    BSONObjBuilder bob(builder->subobjStart(InternalBucketGeoWithinMatchExpression::kName));
    // Serialize the geometry shape.
    BSONObjBuilder withinRegionBob(
        bob.subobjStart(InternalBucketGeoWithinMatchExpression::kWithinRegion));
    if (opts.replacementForLiteralArgs) {
        bob.append(_geoContainer->getGeoElement().fieldName(), *opts.replacementForLiteralArgs);
    } else {
        withinRegionBob.append(_geoContainer->getGeoElement());
    }
    withinRegionBob.doneFast();
    // Serialize the field which is being searched over.
    bob.append(InternalBucketGeoWithinMatchExpression::kField,
               opts.serializeFieldPathFromString(_field));
    bob.doneFast();
}

std::unique_ptr<MatchExpression> InternalBucketGeoWithinMatchExpression::clone() const {
    std::unique_ptr<InternalBucketGeoWithinMatchExpression> next =
        std::make_unique<InternalBucketGeoWithinMatchExpression>(_geoContainer, _field);
    if (getTag()) {
        next->setTag(getTag()->clone());
    }
    return next;
}

}  // namespace mongo
