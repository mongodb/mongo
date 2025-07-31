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

#include "mongo/db/query/compiler/parsers/matcher/expression_geo_parser.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_geo_serializer.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#include <cmath>
#include <limits>
#include <memory>
#include <utility>

#include <s2cellid.h>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::parsers::matcher {
namespace {
Status parseGeoQuery(const BSONObj& obj, GeoExpression& expr) {
    BSONObjIterator outerIt(obj);
    // "within" / "geoWithin" / "geoIntersects"
    BSONElement queryElt = outerIt.next();
    if (outerIt.more()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "can't parse extra field: " << outerIt.next());
    }

    auto keyword = MatchExpressionParser::parsePathAcceptingKeyword(queryElt);
    if (PathAcceptingKeyword::GEO_INTERSECTS == keyword) {
        expr.setPredicate(GeoExpression::INTERSECT);
    } else if (PathAcceptingKeyword::WITHIN == keyword) {
        expr.setPredicate(GeoExpression::WITHIN);
    } else {
        // eoo() or unknown query predicate.
        return Status(ErrorCodes::BadValue,
                      str::stream() << "invalid geo query predicate: " << obj);
    }

    // Parse geometry after predicates.
    if (BSONType::object != queryElt.type())
        return Status(ErrorCodes::BadValue, "geometry must be an object");
    BSONObj geoObj = queryElt.Obj();

    BSONObjIterator geoIt(geoObj);

    while (geoIt.more()) {
        BSONElement elt = geoIt.next();
        // $geoWithin doesn't accept multiple shapes.
        if (expr.getGeometryPtr() && queryElt.fieldNameStringData() == kGeoWithinField) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "$geoWithin doesn't accept multiple shapes "
                                        << queryElt.toString());
        }
        if (elt.fieldNameStringData() == kUniqueDocsField) {
            // Deprecated "$uniqueDocs" field
            LOGV2_WARNING(23847, "Deprecated $uniqueDocs option", "query"_attr = redact(obj));
        } else {
            // The element must be a geo specifier. "$box", "$center", "$geometry", etc.
            auto geoContainer = std::make_shared<GeometryContainer>();
            Status status = geoContainer->parseFromQuery(elt);
            if (!status.isOK())
                return status;
            expr.setGeometry(geoContainer);
        }
    }

    if (expr.getGeometryPtr() == nullptr) {
        return Status(ErrorCodes::BadValue, "geo query doesn't have any geometry");
    }

    return Status::OK();
}

bool parseLegacyQuery(const BSONObj& obj, GeoNearExpression& expr) {
    bool hasGeometry = false;

    // First, try legacy near, e.g.:
    // t.find({ loc : { $nearSphere: [0,0], $minDistance: 1, $maxDistance: 3 }})
    // t.find({ loc : { $nearSphere: [0,0] }})
    // t.find({ loc : { $near : [0, 0, 1] } });
    // t.find({ loc : { $near: { someGeoJSONPoint}})
    // t.find({ loc : { $geoNear: { someGeoJSONPoint}})
    BSONObjIterator it(obj);
    while (it.more()) {
        BSONElement e = it.next();
        StringData fieldName = e.fieldNameStringData();
        if ((fieldName == kNearField) || (fieldName == kGeoNearField) ||
            (fieldName == kNearSphereField)) {
            if (!e.isABSONObj()) {
                return false;
            }

            if (GeoParser::parseQueryPoint(e, expr.centroid.get()).isOK() ||
                GeoParser::parsePointWithMaxDistance(e, expr.centroid.get(), &expr.maxDistance)
                    .isOK()) {
                uassert(18522, "max distance must be non-negative", expr.maxDistance >= 0.0);
                hasGeometry = true;
                expr.isNearSphere = (e.fieldNameStringData() == kNearSphereField);
            } else {
                // We couldn't parse out a query point. This could mean that the provided argument
                // was in the new, not the legacy, format, or it could mean that the point was
                // malformed. If the latter, returning and redirecting to parseNewPoint() will
                // result in a confusing error message. This check is a best effort attempt at
                // inferring what the user was trying to do so that we can return a helpful error if
                // it appears to be an invalid legacy point.
                const auto isLegacyFormat = !e.Obj().hasField(kGeometryField);
                uassert(ErrorCodes::BadValue,
                        str::stream() << "invalid point provided to geo near query: " << e.Obj(),
                        !isLegacyFormat);
            }
        } else if (fieldName == kMinDistanceField) {
            uassert(16893, "$minDistance must be a number", e.isNumber());
            expr.minDistance = e.Number();
            uassert(16894, "$minDistance must be non-negative", expr.minDistance >= 0.0);
        } else if (fieldName == kMaxDistanceField) {
            uassert(16895, "$maxDistance must be a number", e.isNumber());
            expr.maxDistance = e.Number();
            uassert(16896, "$maxDistance must be non-negative", expr.maxDistance >= 0.0);
        } else if (fieldName == kUniqueDocsField) {
            LOGV2_WARNING(23848, "Ignoring deprecated option $uniqueDocs");
        } else {
            // In a query document, $near queries can have no non-geo sibling parameters.
            uasserted(34413,
                      str::stream() << "invalid argument in geo near query: " << e.fieldName());
        }
    }

    return hasGeometry;
}

Status parseNewQuery(const BSONObj& obj, GeoNearExpression& expr) {
    bool hasGeometry = false;

    BSONObjIterator objIt(obj);
    if (!objIt.more()) {
        return Status(ErrorCodes::BadValue, "empty geo near query object");
    }
    BSONElement e = objIt.next();
    // Just one arg. to $geoNear.
    if (objIt.more()) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "geo near accepts just one argument when querying for a GeoJSON "
                          << "point. Extra field found: " << objIt.next());
    }

    // Parse "new" near:
    // t.find({"geo" : {"$near" : {"$geometry": pointA, $minDistance: 1, $maxDistance: 3}}})
    // t.find({"geo" : {"$geoNear" : {"$geometry": pointA, $minDistance: 1, $maxDistance: 3}}})
    if (!e.isABSONObj()) {
        return Status(ErrorCodes::BadValue, "geo near query argument is not an object");
    }

    if (PathAcceptingKeyword::GEO_NEAR != MatchExpressionParser::parsePathAcceptingKeyword(e)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "invalid geo near query operator: " << e.fieldName());
    }

    // Returns true if 'x' is a valid numeric value, that is, a non-negative finite number.
    auto isValidNumericValue = [](double x) -> bool {
        return x >= 0.0 && std::isfinite(x);
    };

    // Iterate over the argument.
    BSONObjIterator it(e.embeddedObject());
    while (it.more()) {
        BSONElement e = it.next();
        StringData fieldName = e.fieldNameStringData();
        if (fieldName == kGeometryField) {
            if (e.isABSONObj()) {
                BSONObj embeddedObj = e.embeddedObject();
                // TODO SERVER-86141: Refactor geoParsing.
                Status status = GeoParser::parseQueryPoint(e, expr.centroid.get());
                if (!status.isOK()) {
                    return Status(ErrorCodes::BadValue,
                                  str::stream()
                                      << "invalid point in geo near query $geometry argument: "
                                      << embeddedObj << "  " << status.reason());
                }
                if (SPHERE != expr.centroid->crs) {
                    return Status(ErrorCodes::BadValue,
                                  str::stream() << "$near requires geojson point, given "
                                                << embeddedObj.toString());
                }
                hasGeometry = true;
            }
        } else if (fieldName == kMinDistanceField) {
            if (!e.isNumber()) {
                return Status(ErrorCodes::BadValue, "$minDistance must be a number");
            }
            expr.minDistance = e.Number();
            if (!isValidNumericValue(expr.minDistance)) {
                return Status(ErrorCodes::BadValue, "$minDistance must be non-negative");
            }
        } else if (fieldName == kMaxDistanceField) {
            if (!e.isNumber()) {
                return Status(ErrorCodes::BadValue, "$maxDistance must be a number");
            }
            expr.maxDistance = e.Number();
            if (!isValidNumericValue(expr.maxDistance)) {
                return Status(ErrorCodes::BadValue, "$maxDistance must be non-negative");
            }
        } else {
            // Return an error if a bad argument was passed inside the query document.
            return Status(ErrorCodes::BadValue,
                          str::stream() << "invalid argument in geo near query: " << e.fieldName());
        }
    }

    if (!hasGeometry) {
        return Status(ErrorCodes::BadValue, "$geometry is required for geo near query");
    }

    return Status::OK();
}
}  // namespace

Status parseGeoExpressionFromBSON(const BSONObj& obj, GeoExpression& expr) {
    // Initialize geoContainer and parse BSON object
    Status status = parseGeoQuery(obj, expr);
    if (!status.isOK())
        return status;

    // Why do we only deal with $within {polygon}?
    // 1. Finding things within a point is silly and only valid
    // for points and degenerate lines/polys.
    //
    // 2. Finding points within a line is easy but that's called intersect.
    // Finding lines within a line is kind of tricky given what S2 gives us.
    // Doing line-within-line is a valid yet unsupported feature,
    // though I wonder if we want to preserve orientation for lines or
    // allow (a,b),(c,d) to be within (c,d),(a,b).  Anyway, punt on
    // this for now.
    if (GeoExpression::WITHIN == expr.getPred() && !expr.getGeometry().supportsContains()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "$within not supported with provided geometry: " << obj);
    }

    // Big polygon with strict winding order is represented as an S2Loop in SPHERE CRS.
    // So converting the query to SPHERE CRS makes things easier than projecting all the data
    // into STRICT_SPHERE CRS.
    if (STRICT_SPHERE == expr.getGeometry().getNativeCRS()) {
        if (!expr.getGeometry().supportsProject(SPHERE)) {
            return Status(ErrorCodes::BadValue, "only polygon supported with strict winding order");
        }
        expr.getGeometryPtr()->projectInto(SPHERE);
    }

    // $geoIntersect queries are hardcoded to *always* be in SPHERE CRS
    // TODO: This is probably bad semantics, should not do this
    if (GeoExpression::INTERSECT == expr.getPred()) {
        if (!expr.getGeometry().supportsProject(SPHERE)) {
            return Status(ErrorCodes::BadValue,
                          str::stream()
                              << "$geoIntersect not supported with provided geometry: " << obj);
        }
        expr.getGeometryPtr()->projectInto(SPHERE);
    }

    return Status::OK();
}

Status parseGeoNearExpressionFromBSON(const BSONObj& obj, GeoNearExpression& expr) {
    Status status = Status::OK();
    expr.centroid.reset(new PointWithCRS());

    if (!parseLegacyQuery(obj, expr)) {
        // Clear out any half-baked data.
        expr.minDistance = 0;
        expr.isNearSphere = false;
        expr.maxDistance = std::numeric_limits<double>::max();
        // ...and try parsing new format.
        status = parseNewQuery(obj, expr);
    }

    if (!status.isOK())
        return status;

    // Fixup the near query for anonoyances caused by $nearSphere
    if (expr.isNearSphere) {
        // The user-provided point can be flat for a spherical query - needs to be projectable
        if (!ShapeProjection::supportsProject(*expr.centroid, SPHERE)) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Legacy point is out of bounds for spherical query");
        }

        expr.unitsAreRadians = SPHERE != expr.centroid->crs;
        // GeoJSON points imply wrapping queries
        expr.isWrappingQuery = SPHERE == expr.centroid->crs;

        // Project the point to a spherical CRS now that we've got the settings we need
        // We need to manually project here since we aren't using GeometryContainer
        ShapeProjection::projectInto(expr.centroid.get(), SPHERE);
    } else {
        expr.unitsAreRadians = false;
        expr.isWrappingQuery = SPHERE == expr.centroid->crs;
    }

    return status;
}

}  // namespace mongo::parsers::matcher
