/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include <s2cellid.h>
#include <s2cellunion.h>
#include <s2regioncoverer.h>

#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/timeseries/timeseries_constants.h"

namespace mongo {

namespace exec::matcher {

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
        if (elem.eoo()) {
            return BSONElement{};
        }
        if (elem.type() != BSONType::Object) {
            return boost::none;
        }
        elem = elem.Obj()[fieldPath.getFieldName(i)];
    }
    return elem;
}

/**
 * The input matches if the bucket document, 'doc', may contain any geo field that is within
 * 'withinRegion'.
 *  - If false, the bucket must not contain any point that is within 'withinRegion'.
 *  - If true, the bucket may or may not contain points that are within the region. The bucket
 * will go through the subsequent "unpack" stage so as to check each point in the bucket in an
 * equivalent $geoWithin operator individually. Always returns true for any bucket containing
 * objects not of type Point.
 */
bool matchesBSONObj(const InternalBucketGeoWithinMatchExpression* expr, const BSONObj& obj) {
    // Look up the path in control.min and control.max.
    // If it goes through an array, return true to avoid dealing with implicit array traversal.
    // If it goes through a scalar, return true to avoid dealing with mixed types.
    auto controlMinElmOptional =
        derefPath(obj, timeseries::kControlMinFieldNamePrefix + expr->getField());
    if (!controlMinElmOptional) {
        return true;
    }
    auto controlMaxElmOptional =
        derefPath(obj, timeseries::kControlMaxFieldNamePrefix + expr->getField());
    if (!controlMaxElmOptional) {
        return true;
    }
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

    if (controlMinElm.type() != controlMaxElm.type()) {
        return true;
    }
    // Returns true if the bucket should be unpacked and all the data within the bucket should be
    // checked later.
    auto parseMinMaxPoint = [](const BSONElement& elem, PointWithCRS* out) -> bool {
        auto geoObj = elem.embeddedObject();
        if (Array == elem.type() || geoObj.firstElement().isNumber()) {
            // Legacy Point.
            if (!GeoParser::parseLegacyPoint(elem, out, true).isOK()) {
                return true;
            }
        } else {
            // If the bucket contains GeoJSON objects of types other than 'Points' we cannot be sure
            // whether this bucket contains data is within the provided region.
            if (!geoObj.hasField(GEOJSON_TYPE) ||
                geoObj[GEOJSON_TYPE].String() != GEOJSON_TYPE_POINT) {
                return true;
            }
            // GeoJSON Point.
            if (!GeoParser::parseGeoJSONPoint(geoObj, out).isOK()) {
                return true;
            }
        }
        return false;
    };

    PointWithCRS minPoint;
    PointWithCRS maxPoint;
    const CRS crs = expr->getGeoContainer().getNativeCRS();

    if (parseMinMaxPoint(controlMinElm, &minPoint) || parseMinMaxPoint(controlMaxElm, &maxPoint)) {
        return true;
    }
    if (minPoint.crs != maxPoint.crs || crs != minPoint.crs) {
        return true;
    }
    if (crs == FLAT && expr->getGeoContainer().hasR2Region()) {
        // Construct a 2D Box for legacy coordinate pairs. This box is used to check if the box may
        // contain points that are within _geoContainer.
        const Box bbox2D(minPoint.oldPoint, maxPoint.oldPoint);
        if (expr->getGeoContainer().getR2Region().fastDisjoint(bbox2D)) {
            return false;
        }
    }

    if (crs == SPHERE && expr->getGeoContainer().hasS2Region()) {
        const S2LatLngRect rect(S2LatLng(minPoint.point), S2LatLng(maxPoint.point));

        S2RegionCoverer coverer;
        S2CellUnion cellUnionRect;
        S2CellUnion cellUnionGeo;
        coverer.GetCellUnion(rect, &cellUnionRect);
        coverer.GetCellUnion(expr->getGeoContainer().getS2Region(), &cellUnionGeo);

        if (!cellUnionRect.Intersects(&cellUnionGeo)) {
            return false;
        }
    }

    return true;
}

}  // namespace

void MatchExpressionEvaluator::visit(const InternalBucketGeoWithinMatchExpression* expr) {
    _result = matchesBSONObj(expr, _doc->toBSON());
}

}  // namespace exec::matcher
}  // namespace mongo
