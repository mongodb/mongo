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


#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/platform/basic.h"

#include "third_party/s2/s2cellid.h"
#include "third_party/s2/s2cellunion.h"
#include "third_party/s2/s2regioncoverer.h"

namespace mongo {
constexpr StringData InternalBucketGeoWithinMatchExpression::kName;

void InternalBucketGeoWithinMatchExpression::debugString(StringBuilder& debug,
                                                         int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);

    BSONObjBuilder builder;
    serialize(&builder, true);
    debug << builder.obj().toString() << "\n";

    const auto* tag = getTag();
    if (tag) {
        debug << " ";
        tag->debugString(&debug);
    }

    debug << "\n";
}

bool InternalBucketGeoWithinMatchExpression::equivalent(const MatchExpression* expr) const {
    if (matchType() != expr->matchType()) {
        return false;
    }

    const auto* other = static_cast<const InternalBucketGeoWithinMatchExpression*>(expr);

    return SimpleBSONObjComparator::kInstance.evaluate(
               _geoContainer->getGeoElement().Obj() ==
               other->getGeoContainer()->getGeoElement().Obj()) &&
        _field == other->getField();
}

bool InternalBucketGeoWithinMatchExpression::matches(const MatchableDocument* doc,
                                                     MatchDetails* details) const {
    return _matchesBSONObj(doc->toBSON());
}

bool InternalBucketGeoWithinMatchExpression::_matchesBSONObj(const BSONObj& obj) const {
    auto controlMinElm = dotted_path_support::extractElementAtPath(
        obj, str::stream() << timeseries::kControlMinFieldNamePrefix << _field);
    auto controlMaxElm = dotted_path_support::extractElementAtPath(
        obj, str::stream() << timeseries::kControlMaxFieldNamePrefix << _field);

    std::string path = str::stream() << timeseries::kControlMinFieldNamePrefix << _field;

    if (controlMinElm.eoo() || controlMaxElm.eoo() || !controlMinElm.isABSONObj() ||
        !controlMaxElm.isABSONObj())
        return false;

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
                                                       bool includePath) const {
    BSONObjBuilder bob(builder->subobjStart(InternalBucketGeoWithinMatchExpression::kName));
    BSONObjBuilder withinRegionBob(
        bob.subobjStart(InternalBucketGeoWithinMatchExpression::kWithinRegion));
    withinRegionBob.append(_geoContainer->getGeoElement());
    withinRegionBob.doneFast();
    bob.append(InternalBucketGeoWithinMatchExpression::kField, _field);
    bob.doneFast();
}

std::unique_ptr<MatchExpression> InternalBucketGeoWithinMatchExpression::shallowClone() const {
    std::unique_ptr<InternalBucketGeoWithinMatchExpression> next =
        std::make_unique<InternalBucketGeoWithinMatchExpression>(_geoContainer, _field);
    if (getTag()) {
        next->setTag(getTag()->clone());
    }
    return next;
}

}  // namespace mongo
