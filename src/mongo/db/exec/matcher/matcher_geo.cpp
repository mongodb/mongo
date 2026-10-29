// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/timeseries/timeseries_constants.h"

#include <s2cellid.h>
#include <s2cellunion.h>
#include <s2regioncoverer.h>

namespace mongo {

namespace exec::matcher {

bool geoContains(const GeometryContainer& queryGeom,
                 const GeoExpression::Predicate& queryPredicate,
                 GeometryContainer& otherContainer) {
    otherContainer.projectInto(queryGeom.getNativeCRS());
    if (GeoExpression::WITHIN == queryPredicate) {
        return queryGeom.contains(otherContainer);
    } else {
        MONGO_verify(GeoExpression::INTERSECT == queryPredicate);
        return queryGeom.intersects(otherContainer);
    }
}

bool geoContains(const GeometryContainer& queryGeom,
                 const GeoExpression::Predicate& queryPredicate,
                 bool skipValidation,
                 const BSONElement& e,
                 boost::optional<S2IndexVersion> indexVersion) {
    if (!e.isABSONObj()) {
        return false;
    }

    GeometryContainer geometry;
    if (!geometry.parseFromStorage(e, skipValidation, indexVersion).isOK()) {
        return false;
    }

    // Never match big polygon
    if (geometry.getNativeCRS() == STRICT_SPHERE) {
        return false;
    }

    // Project this geometry into the CRS of the larger geometry.

    // In the case of index validation, we are projecting the geometry of the query
    // into the CRS of the index to confirm that the index region covers/includes
    // the region described by the predicate.

    if (!geometry.supportsProject(queryGeom.getNativeCRS())) {
        return false;
    }

    return geoContains(queryGeom, queryPredicate, geometry);
}


bool matchesGeoContainer(const GeoMatchExpression* expr, const GeometryContainer& input) {
    // Never match big polygon
    if (input.getNativeCRS() == STRICT_SPHERE) {
        return false;
    }

    const auto& query = expr->getGeoExpression();

    // Project this geometry into the CRS of the larger geometry.

    // In the case of index validation, we are projecting the geometry of the query
    // into the CRS of the index to confirm that the index region convers/includes
    // the region described by the predicate.

    if (!input.supportsProject(query.getGeometry().getNativeCRS())) {
        return false;
    }

    GeometryContainer geometry{input};
    return geoContains(query.getGeometry(), query.getPred(), geometry);
}

void MatchesSingleElementEvaluator::visit(const GeoMatchExpression* expr) {
    const auto& query = expr->getGeoExpression();
    _result = geoContains(query.getGeometry(),
                          query.getPred(),
                          expr->getCanSkipValidation(),
                          _elem,
                          expr->get2dsphereIndexVersion());
}

void MatchesSingleElementEvaluator::visit(const TwoDPtInAnnulusExpression* expr) {
    if (!_elem.isABSONObj()) {
        _result = false;
        return;
    }

    PointWithCRS point;
    if (!GeoParser::parseStoredPoint(_elem, &point).isOK()) {
        _result = false;
        return;
    }

    _result = expr->getAnnulus().contains(point.oldPoint);
}

/**
 * Stub implementation that should never be called, since geoNear execution requires an
 * appropriate geo index.
 */
void MatchesSingleElementEvaluator::visit(const GeoNearMatchExpression* expr) {
    _result = true;
}

void MatchesSingleElementEvaluator::visit(const InternalBucketGeoWithinMatchExpression* expr) {
    // This expression should only be used to match full documents
    MONGO_UNREACHABLE_TASSERT(10071003);
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
        if (elem.eoo()) {
            return BSONElement{};
        }
        if (elem.type() != BSONType::object) {
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
        derefPath(obj, std::string{timeseries::kControlMinFieldNamePrefix} + expr->getField());
    if (!controlMinElmOptional) {
        return true;
    }
    auto controlMaxElmOptional =
        derefPath(obj, std::string{timeseries::kControlMaxFieldNamePrefix} + expr->getField());
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
    auto parseMinMaxPoint = [indexVersion = expr->getIndexVersion()](const BSONElement& elem,
                                                                     PointWithCRS* out) -> bool {
        if (!indexVersion && elem.type() == BSONType::object &&
            elem.Obj().firstElement().isNumber()) {
            // Without an index version (e.g., on a mongos without catalog access), we cannot
            // resolve the V3 vs V4+ parsing ambiguity for objects whose first field is numeric:
            // V3 tries legacy-point first; V4+ tries GeoJSON first. Conservatively unpack the
            // bucket to avoid discarding data that the index correctly returned.
            return true;
        }
        auto geoObj = elem.embeddedObject();
        GeometryContainer geometry;
        Status status = geometry.parseFromStorage(elem, false, indexVersion);
        if (!status.isOK()) {
            return true;  // Parsing from storage failed, we should unpack the bucket to be safe.
        }
        if (!geometry.isPoint()) {
            // The stored value didn't parse as a point, so we can't do a
            // bounding-box comparison on the control min/max; unpack the bucket.
            return true;
        }
        *out = geometry.getPoint();
        return false;  // No need to unpack.
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
        // Use R1Interval::FromPointPair for latitude to handle a floating-point
        // precision edge case. Converting GeoJSON to S2Point computes
        // x = cos(lat)*cos(lng), y = cos(lat)*sin(lng). Recovering latitude uses
        // sqrt(x^2 + y^2), which depends on cos^2(lng) + sin^2(lng). In floating
        // point this sum is not exactly 1.0 and varies by longitude, so two points
        // at the same latitude but different longitudes can recover slightly
        // different latitudes. This can flip min > max by the smallest
        // representable amount, which fails R1Interval's validity check.
        // FromPointPair swaps if needed, avoiding the assertion.
        // Longitude does not have an equivalent precision issue: atan2(y,x) always
        // returns values in [-pi, pi], and S1Interval permits lo > hi for
        // antimeridian wrapping. We use the directed S1Interval constructor instead
        // of FromPointPair because FromPointPair always picks the shorter arc
        // (<= 180 degrees), which would shrink bounding boxes wider than 180
        // degrees to their complement.
        const S2LatLng minLatLng(minPoint.point);
        const S2LatLng maxLatLng(maxPoint.point);
        const S2LatLngRect rect(
            R1Interval::FromPointPair(minLatLng.lat().radians(), maxLatLng.lat().radians()),
            S1Interval(minLatLng.lng().radians(), maxLatLng.lng().radians()));

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
