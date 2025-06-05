/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/db/geo/geoparser.h"

#include <cstddef>

#include <s1angle.h>
#include <s1interval.h>
#include <s2.h>
#include <s2cap.h>
#include <s2cell.h>
#include <s2cellid.h>
#include <s2latlng.h>
#include <s2loop.h>
#include <s2polygon.h>
#include <s2polyline.h>

#include <util/math/vector3-inl.h>
#include <util/math/vector3.h>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/clonable_ptr.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/db/geo/big_polygon.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/transitional_tools_do_not_use/vector_spooling.h"

#include <cmath>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kGeo


#define BAD_VALUE(error) Status(ErrorCodes::BadValue, str::stream() << error)

namespace mongo {

namespace dps = ::mongo::bson;

// Convenience function to extract flat point coordinates from enclosing element.
// Note, coordinate elements must not outlive the parent element.
Status GeoParser::parseFlatPointCoordinates(const BSONElement& elem,
                                            BSONElement& x,
                                            BSONElement& y,
                                            bool allowAddlFields /* = false */) {
    if (!elem.isABSONObj()) {
        return BAD_VALUE("Point must be an array or object, instead got type "
                         << typeName(elem.type()));
    }

    BSONObjIterator it(elem.Obj());
    x = it.next();
    if (!x.isNumber()) {
        return BAD_VALUE("Point must only contain numeric elements, instead got type "
                         << typeName(x.type()));
    }
    y = it.next();
    if (!y.isNumber()) {
        return BAD_VALUE("Point must only contain numeric elements, instead got type "
                         << typeName(y.type()));
    }
    if (!allowAddlFields && it.more()) {
        return BAD_VALUE("Point must only contain two numeric elements");
    }
    return Status::OK();
}

// Convenience function to extract point coordinates and distance from enclosing element.
// Note, coordinate and distance elements must not outlive the parent element.
Status GeoParser::parseLegacyPointWithMaxDistance(const BSONElement& elem,
                                                  BSONElement& lat,
                                                  BSONElement& lng,
                                                  BSONElement& maxDist) {
    if (!elem.isABSONObj()) {
        return BAD_VALUE("Point and distance must be an array or object, instead got type "
                         << typeName(elem.type()));
    }

    BSONObjIterator it(elem.Obj());
    if (!it.more()) {
        return BAD_VALUE("Point with max distance must contain exactly three numeric elements");
    }
    lat = it.next();
    if (!lat.isNumber()) {
        return BAD_VALUE(
            "Point with max distance must only contain numeric elements, instead got type "
            << typeName(lat.type()));
    }

    if (!it.more()) {
        return BAD_VALUE("Point with max distance must contain exactly three numeric elements");
    }
    lng = it.next();
    if (!lng.isNumber()) {
        return BAD_VALUE(
            "Point with max distance must only contain numeric elements, instead got type "
            << typeName(lng.type()));
    }

    if (!it.more()) {
        return BAD_VALUE("Point with max distance must contain exactly three numeric elements");
    }
    maxDist = it.next();
    if (!maxDist.isNumber()) {
        return BAD_VALUE(
            "Point with max distance must only contain numeric elements, instead got type "
            << typeName(maxDist.type()));
    }

    if (it.more()) {
        return BAD_VALUE("Point with max distance must contain exactly three numeric elements");
    }
    return Status::OK();
}

static Status parseFlatPoint(const BSONElement& elem, Point* out, bool allowAddlFields = false) {
    BSONElement x, y;
    auto status = GeoParser::parseFlatPointCoordinates(elem, x, y, allowAddlFields);
    if (!status.isOK()) {
        return status;
    }

    out->x = x.number();
    out->y = y.number();
    // Point coordinates must be finite numbers, neither NaN or infinite.
    if (!std::isfinite(out->x) || !std::isfinite(out->y)) {
        return BAD_VALUE("Point coordinates must be finite numbers");
    }
    return Status::OK();
}

Status GeoParser::parseLegacyPoint(const BSONElement& elem,
                                   PointWithCRS* out,
                                   bool allowAddlFields) {
    out->crs = FLAT;
    return parseFlatPoint(elem, &out->oldPoint, allowAddlFields);
}

static Status coordToPoint(double lng, double lat, S2Point* out) {
    // We don't rely on drem to clean up non-sane points.  We just don't let them become
    // spherical.
    if (!isValidLngLat(lng, lat))
        return BAD_VALUE("Longitude/latitude is out of bounds, lng: " << lng << " lat: " << lat);
    // Note that it's (lat, lng) for S2 but (lng, lat) for MongoDB.
    S2LatLng ll = S2LatLng::FromDegrees(lat, lng).Normalized();
    // This shouldn't happen since we should only have valid lng/lats.
    if (!ll.is_valid()) {
        std::stringstream ss;
        ss << "coords invalid after normalization, lng = " << lng << " lat = " << lat << endl;
        uasserted(17125, ss.str());
    }
    *out = ll.ToPoint();
    return Status::OK();
}

static Status parseGeoJSONCoordinate(const BSONElement& elem, S2Point* out) {
    if (BSONType::array != elem.type()) {
        return BAD_VALUE("GeoJSON coordinates must be an array, instead got type "
                         << typeName(elem.type()));
    }
    Point p;
    // GeoJSON allows extra elements, e.g. altitude.
    Status status = parseFlatPoint(elem, &p, true);
    if (!status.isOK())
        return status;

    status = coordToPoint(p.x, p.y, out);
    return status;
}

// "coordinates": [ [100.0, 0.0], [101.0, 1.0] ]
static Status parseArrayOfCoordinates(const BSONElement& elem, vector<S2Point>* out) {
    if (BSONType::array != elem.type()) {
        return BAD_VALUE("GeoJSON coordinates must be an array of coordinates, instead got type "
                         << typeName(elem.type()));
    }
    BSONObjIterator it(elem.Obj());
    // Iterate all coordinates in array.
    while (it.more()) {
        S2Point p;
        Status status = parseGeoJSONCoordinate(it.next(), &p);
        if (!status.isOK())
            return status;
        out->push_back(p);
    }
    return Status::OK();
}

static void eraseDuplicatePoints(vector<S2Point>* vertices) {
    // Duplicates can't exist in a vector of 0 or 1 elements, and we want to be careful about
    // possible underflow of size - 1 in the next block.
    if (vertices->size() < 2) {
        return;
    }

    size_t i = 0;
    while (i < vertices->size() - 1) {
        if ((*vertices)[i] == (*vertices)[i + 1]) {
            vertices->erase(vertices->begin() + i + 1);
            // We could have > 2 adjacent identical vertices, and must examine i again, so we don't
            // increment the iterator.
        } else {
            ++i;
        }
    }
}

static Status isLoopClosed(const vector<S2Point>& loop, const BSONElement loopElt) {
    if (loop.empty()) {
        return BAD_VALUE("Loop has no vertices: " << loopElt.toString(false));
    }

    if (loop[0] != loop[loop.size() - 1]) {
        return BAD_VALUE("Loop is not closed, first vertex does not equal last vertex: "
                         << loopElt.toString(false));
    }

    return Status::OK();
}

static Status parseGeoJSONPolygonCoordinates(const BSONElement& elem,
                                             bool skipValidation,
                                             S2Polygon* out) {
    if (BSONType::array != elem.type()) {
        return BAD_VALUE("Polygon coordinates must be an array, instead got type "
                         << typeName(elem.type()));
    }

    std::vector<std::unique_ptr<S2Loop>> loops;
    Status status = Status::OK();
    string err;

    BSONObjIterator it(elem.Obj());
    // Iterate all loops of the polygon.
    while (it.more()) {
        // Parse the array of vertices of a loop.
        BSONElement coordinateElt = it.next();
        vector<S2Point> points;
        status = parseArrayOfCoordinates(coordinateElt, &points);
        if (!status.isOK())
            return status;

        // Check if the loop is closed.
        status = isLoopClosed(points, coordinateElt);
        if (!status.isOK())
            return status;

        eraseDuplicatePoints(&points);
        // Drop the duplicated last point.
        points.resize(points.size() - 1);

        // At least 3 vertices.
        if (points.size() < 3) {
            return BAD_VALUE("Loop must have at least 3 different vertices, "
                             << points.size() << " unique vertices were provided: "
                             << coordinateElt.toString(false));
        }

        loops.push_back(std::make_unique<S2Loop>(points));
        S2Loop* loop = loops.back().get();

        // Check whether this loop is valid if vaildation hasn't been already done on 2dSphere index
        // insertion which is controlled by the 'skipValidation' flag.
        // 1. At least 3 vertices.
        // 2. All vertices must be unit length. Guaranteed by parsePoints().
        // 3. Loops are not allowed to have any duplicate vertices.
        // 4. Non-adjacent edges are not allowed to intersect.
        if (!skipValidation && !loop->IsValid(&err)) {
            return BAD_VALUE("Loop is not valid: " << coordinateElt.toString(false) << " " << err);
        }
        // If the loop is more than one hemisphere, invert it.
        loop->Normalize();

        // Check the first loop must be the exterior ring and any others must be
        // interior rings or holes.
        if (!skipValidation && loops.size() > 1 && !loops[0]->Contains(loop)) {
            return BAD_VALUE(
                "Secondary loops not contained by first exterior loop - "
                "secondary loops must be holes: "
                << coordinateElt.toString(false)
                << " first loop: " << elem.Obj().firstElement().toString(false));
        }
    }

    if (loops.empty()) {
        return BAD_VALUE("Polygon has no loops.");
    }


    // Check if the given loops form a valid polygon.
    // 1. If a loop contains an edge AB, then no other loop may contain AB or BA.
    // 2. No loop covers more than half of the sphere.
    // 3. No two loops cross.
    if (!skipValidation &&
        !S2Polygon::IsValid(transitional_tools_do_not_use::unspool_vector(loops), &err))
        return BAD_VALUE("Polygon isn't valid: " << err << " " << elem.toString(false));

    // Given all loops are valid / normalized and S2Polygon::IsValid() above returns true.
    // The polygon must be valid. See S2Polygon member function IsValid().

    {
        // Transfer ownership of the loops and clears loop vector.
        std::vector<S2Loop*> rawLoops = transitional_tools_do_not_use::leak_vector(loops);
        out->Init(&rawLoops);
    }

    if (skipValidation)
        return Status::OK();

    // Check if every loop of this polygon shares at most one vertex with
    // its parent loop.
    if (!out->IsNormalized(&err))
        // "err" looks like "Loop 1 shares more than one vertex with its parent loop 0"
        return BAD_VALUE(err << ": " << elem.toString(false));

    // S2Polygon contains more than one ring, which is allowed by S2, but not by GeoJSON.
    //
    // Loops are indexed according to a preorder traversal of the nesting hierarchy.
    // GetLastDescendant() returns the index of the last loop that is contained within
    // a given loop. We guarantee that the first loop is the exterior ring.
    if (out->GetLastDescendant(0) < out->num_loops() - 1) {
        return BAD_VALUE("Only one exterior polygon loop is allowed: " << elem.toString(false));
    }

    // In GeoJSON, only one nesting is allowed.
    // The depth of a loop is set by polygon according to the nesting hierarchy of polygon,
    // so the exterior ring's depth is 0, a hole in it is 1, etc.
    for (int i = 0; i < out->num_loops(); i++) {
        if (out->loop(i)->depth() > 1) {
            return BAD_VALUE("Polygon interior loops cannot be nested: " << elem.toString(false));
        }
    }
    return Status::OK();
}

static Status parseBigSimplePolygonCoordinates(const BSONElement& elem, BigSimplePolygon* out) {
    if (BSONType::array != elem.type()) {
        return BAD_VALUE("Coordinates of polygon must be an array, instead got type "
                         << typeName(elem.type()));
    }


    const vector<BSONElement>& coordinates = elem.Array();
    // Only one loop is allowed in a BigSimplePolygon
    if (coordinates.size() != 1) {
        return BAD_VALUE("Only one simple loop is allowed in a big polygon, instead provided "
                         << coordinates.size() << " loops: " << elem.toString(false));
    }

    vector<S2Point> exteriorVertices;
    Status status = Status::OK();
    string err;

    status = parseArrayOfCoordinates(coordinates.front(), &exteriorVertices);
    if (!status.isOK())
        return status;

    status = isLoopClosed(exteriorVertices, coordinates.front());
    if (!status.isOK())
        return status;

    eraseDuplicatePoints(&exteriorVertices);

    // The last point is duplicated.  We drop it, since S2Loop expects no
    // duplicate points
    exteriorVertices.resize(exteriorVertices.size() - 1);

    // At least 3 vertices.
    if (exteriorVertices.size() < 3) {
        return BAD_VALUE("Loop must have at least 3 different vertices, "
                         << exteriorVertices.size()
                         << " unique vertices were provided: " << elem.toString(false));
    }

    std::unique_ptr<S2Loop> loop(new S2Loop(exteriorVertices));
    // Check whether this loop is valid.
    if (!loop->IsValid(&err)) {
        return BAD_VALUE("Loop is not valid: " << elem.toString(false) << " " << err);
    }

    out->Init(loop.release());
    return Status::OK();
}

// Parse "crs" field of BSON object.
// "crs": {
//   "type": "name",
//   "properties": {
//     "name": "urn:ogc:def:crs:OGC:1.3:CRS84"
//    }
// }
static Status parseGeoJSONCRS(const BSONObj& obj, CRS* crs, bool allowStrictSphere = false) {
    *crs = SPHERE;

    BSONElement crsElt = obj[kCrsField];
    // "crs" field doesn't exist, return the default SPHERE
    if (crsElt.eoo()) {
        return Status::OK();
    }

    if (!crsElt.isABSONObj()) {
        return BAD_VALUE("GeoJSON CRS must be an object, instead got type "
                         << typeName(crsElt.type()));
    }
    BSONObj crsObj = crsElt.embeddedObject();

    // "type": "name"
    if (BSONType::string != crsObj[kCrsTypeField].type() ||
        kCrsNameField != crsObj[kCrsTypeField].String())
        return BAD_VALUE("GeoJSON CRS must have field \"type\": \"name\"");

    // "properties"
    BSONElement propertiesElt = crsObj[kCrsPropertiesField];
    if (!propertiesElt.isABSONObj()) {
        return BAD_VALUE("CRS must have field \"properties\" which is an object, instead got type "
                         << typeName(propertiesElt.type()));
    }
    BSONObj propertiesObj = propertiesElt.embeddedObject();
    if (BSONType::string != propertiesObj[kPropertiesNameField].type()) {
        return BAD_VALUE("In CRS, \"properties.name\" must be a string, instead got type "
                         << typeName(propertiesObj[kPropertiesNameField].type()));
    }

    const string& name = propertiesObj[kPropertiesNameField].String();
    if (CRS_CRS84 == name || CRS_EPSG_4326 == name) {
        *crs = SPHERE;
    } else if (CRS_STRICT_WINDING == name) {
        if (!allowStrictSphere) {
            return BAD_VALUE("Strict winding order CRS is only supported by polygon");
        }
        *crs = STRICT_SPHERE;
    } else {
        return BAD_VALUE("Unknown CRS name: " << name);
    }
    return Status::OK();
}

// Parse "coordinates" field of GeoJSON LineString
// e.g. "coordinates": [ [100.0, 0.0], [101.0, 1.0] ]
// Or a line in "coordinates" field of GeoJSON MultiLineString
static Status parseGeoJSONLineCoordinates(const BSONElement& elem,
                                          bool skipValidation,
                                          S2Polyline* out) {
    vector<S2Point> vertices;
    Status status = parseArrayOfCoordinates(elem, &vertices);
    if (!status.isOK())
        return status;

    eraseDuplicatePoints(&vertices);
    if (!skipValidation) {
        if (vertices.size() < 2)
            return BAD_VALUE("GeoJSON LineString must have at least 2 vertices, instead got "
                             << vertices.size() << " vertices: " << elem.toString(false));

        string err;
        if (!S2Polyline::IsValid(vertices, &err))
            return BAD_VALUE("GeoJSON LineString is not valid: " << err << " "
                                                                 << elem.toString(false));
    }
    out->Init(vertices);
    return Status::OK();
}

// Parse legacy point or GeoJSON point, used by geo near.
// Only stored legacy points allow additional fields.
Status parsePoint(const BSONElement& elem, PointWithCRS* out, bool allowAddlFields) {
    if (!elem.isABSONObj()) {
        return BAD_VALUE("Point must be an array or object, instead got type "
                         << typeName(elem.type()));
    }
    BSONObj obj = elem.Obj();
    // location: [1, 2] or location: {x: 1, y:2}
    if (BSONType::array == elem.type() || obj.firstElement().isNumber()) {
        // Legacy point
        return GeoParser::parseLegacyPoint(elem, out, allowAddlFields);
    }

    // GeoJSON point. location: { type: "Point", coordinates: [1, 2] }
    return GeoParser::parseGeoJSONPoint(obj, out);
}

/** exported **/
Status GeoParser::parseStoredPoint(const BSONElement& elem, PointWithCRS* out) {
    return parsePoint(elem, out, true);
}

Status GeoParser::parseQueryPoint(const BSONElement& elem, PointWithCRS* out) {
    return parsePoint(elem, out, false);
}

Status GeoParser::parseLegacyBox(const BSONObj& obj, BoxWithCRS* out) {
    Point ptA, ptB;
    Status status = Status::OK();

    BSONObjIterator coordIt(obj);
    status = parseFlatPoint(coordIt.next(), &ptA);
    if (!status.isOK()) {
        return status;
    }
    status = parseFlatPoint(coordIt.next(), &ptB);
    if (!status.isOK()) {
        return status;
    }
    // XXX: VERIFY AREA >= 0

    out->box.init(ptA, ptB);
    out->crs = FLAT;
    return status;
}

Status GeoParser::parseLegacyPolygon(const BSONObj& obj, PolygonWithCRS* out) {
    BSONObjIterator coordIt(obj);
    vector<Point> points;
    while (coordIt.more()) {
        Point p;
        // A coordinate
        Status status = parseFlatPoint(coordIt.next(), &p);
        if (!status.isOK())
            return status;
        points.push_back(p);
    }
    if (points.size() < 3)
        return BAD_VALUE("Polygon must have at least 3 points, instead got " << points.size()
                                                                             << " vertices");
    out->oldPolygon.init(points);
    out->crs = FLAT;
    return Status::OK();
}

// { "type": "Point", "coordinates": [100.0, 0.0] }
Status GeoParser::parseGeoJSONPoint(const BSONObj& obj, PointWithCRS* out) {
    if (obj.hasField(GEOJSON_TYPE)) {
        // GeoJSON Point must explicitly specify the type as "Point".
        auto typeVal = GeoParser::parseGeoJSONType(obj);
        if (GeoParser::GEOJSON_POINT != typeVal) {
            return BAD_VALUE("Expected geojson geometry with type Point, but got type "
                             << GeoParser::geoJSONTypeEnumToString(typeVal));
        }
    }

    Status status = Status::OK();
    // "crs"
    status = parseGeoJSONCRS(obj, &out->crs);
    if (!status.isOK())
        return status;

    // "coordinates"
    status = parseFlatPoint(obj[GEOJSON_COORDINATES], &out->oldPoint, true);
    if (!status.isOK())
        return status;

    // Projection
    out->crs = FLAT;
    if (!ShapeProjection::supportsProject(*out, SPHERE))
        return BAD_VALUE("Longitude/latitude is out of bounds, lng: " << out->oldPoint.x << " lat: "
                                                                      << out->oldPoint.y);
    ShapeProjection::projectInto(out, SPHERE);
    return Status::OK();
}

// { "type": "LineString", "coordinates": [ [100.0, 0.0], [101.0, 1.0] ] }
Status GeoParser::parseGeoJSONLine(const BSONObj& obj, bool skipValidation, LineWithCRS* out) {
    Status status = Status::OK();
    // "crs"
    status = parseGeoJSONCRS(obj, &out->crs);
    if (!status.isOK())
        return status;

    // "coordinates"
    status = parseGeoJSONLineCoordinates(obj[GEOJSON_COORDINATES], skipValidation, &out->line);
    if (!status.isOK())
        return status;

    return Status::OK();
}

Status GeoParser::parseGeoJSONPolygon(const BSONObj& obj,
                                      bool skipValidation,
                                      PolygonWithCRS* out) {
    const BSONElement coordinates = obj[GEOJSON_COORDINATES];

    Status status = Status::OK();
    // "crs", allow strict sphere
    status = parseGeoJSONCRS(obj, &out->crs, true);
    if (!status.isOK())
        return status;

    // "coordinates"
    if (out->crs == SPHERE) {
        out->s2Polygon.reset(new S2Polygon());
        status = parseGeoJSONPolygonCoordinates(coordinates, skipValidation, out->s2Polygon.get());
    } else if (out->crs == STRICT_SPHERE) {
        out->bigPolygon.reset(new BigSimplePolygon());
        status = parseBigSimplePolygonCoordinates(coordinates, out->bigPolygon.get());
    }
    return status;
}

Status GeoParser::parseMultiPoint(const BSONObj& obj, MultiPointWithCRS* out) {
    Status status = Status::OK();
    status = parseGeoJSONCRS(obj, &out->crs);
    if (!status.isOK())
        return status;

    out->points.clear();
    BSONElement coordElt = dps::extractElementAtDottedPath(obj, GEOJSON_COORDINATES);
    status = parseArrayOfCoordinates(coordElt, &out->points);
    if (!status.isOK())
        return status;

    if (0 == out->points.size())
        return BAD_VALUE("MultiPoint coordinates must have at least 1 element");
    out->cells.resize(out->points.size());
    for (size_t i = 0; i < out->points.size(); ++i) {
        out->cells[i] = S2Cell(out->points[i]);
    }

    return Status::OK();
}

Status GeoParser::parseMultiLine(const BSONObj& obj, bool skipValidation, MultiLineWithCRS* out) {
    Status status = Status::OK();
    status = parseGeoJSONCRS(obj, &out->crs);
    if (!status.isOK())
        return status;

    BSONElement coordElt = dps::extractElementAtDottedPath(obj, GEOJSON_COORDINATES);
    if (BSONType::array != coordElt.type()) {
        return BAD_VALUE("MultiLineString coordinates must be an array, instead got type "
                         << typeName(coordElt.type()));
    }


    out->lines.clear();
    auto& lines = out->lines;

    BSONObjIterator it(coordElt.Obj());

    // Iterate array
    while (it.more()) {
        lines.push_back(std::make_unique<S2Polyline>());
        status = parseGeoJSONLineCoordinates(it.next(), skipValidation, lines.back().get());
        if (!status.isOK())
            return status;
    }
    if (0 == lines.size())
        return BAD_VALUE("MultiLineString coordinates must have at least 1 element");

    return Status::OK();
}

Status GeoParser::parseMultiPolygon(const BSONObj& obj,
                                    bool skipValidation,
                                    MultiPolygonWithCRS* out) {
    Status status = Status::OK();
    status = parseGeoJSONCRS(obj, &out->crs);
    if (!status.isOK())
        return status;

    BSONElement coordElt = dps::extractElementAtDottedPath(obj, GEOJSON_COORDINATES);
    if (BSONType::array != coordElt.type()) {
        return BAD_VALUE("MultiPolygon coordinates must be an array, instead got type "
                         << typeName(coordElt.type()));
    }
    out->polygons.clear();
    auto& polygons = out->polygons;

    BSONObjIterator it(coordElt.Obj());
    // Iterate array
    while (it.more()) {
        polygons.push_back(std::make_unique<S2Polygon>());
        status = parseGeoJSONPolygonCoordinates(it.next(), skipValidation, polygons.back().get());
        if (!status.isOK())
            return status;
    }
    if (0 == polygons.size())
        return BAD_VALUE("MultiPolygon coordinates must have at least 1 element");

    return Status::OK();
}

Status GeoParser::parseLegacyCenter(const BSONObj& obj, CapWithCRS* out) {
    BSONObjIterator objIt(obj);

    // Center
    BSONElement center = objIt.next();
    Status status = parseFlatPoint(center, &out->circle.center);
    if (!status.isOK())
        return status;

    // Radius
    BSONElement radius = objIt.next();
    // radius >= 0 and is not NaN
    if (!radius.isNumber() || !(radius.number() >= 0))
        return BAD_VALUE("Radius must be a non-negative number: " << radius.toString(false));

    // No more
    if (objIt.more())
        return BAD_VALUE("Only 2 fields allowed for circular region, but more were provided");

    out->circle.radius = radius.number();
    out->crs = FLAT;
    return Status::OK();
}

Status GeoParser::parseCenterSphere(const BSONObj& obj, CapWithCRS* out) {
    BSONObjIterator objIt(obj);

    // Center
    BSONElement center = objIt.next();
    Point p;
    // Check the object has and only has 2 numbers.
    Status status = parseFlatPoint(center, &p);
    if (!status.isOK())
        return status;

    S2Point centerPoint;
    status = coordToPoint(p.x, p.y, &centerPoint);
    if (!status.isOK())
        return status;

    // Radius
    BSONElement radiusElt = objIt.next();
    // radius >= 0 and is not NaN
    if (!radiusElt.isNumber() || !(radiusElt.number() >= 0)) {
        return BAD_VALUE("Radius must be a non-negative number: " << radiusElt.toString(false));
    }

    double radius = radiusElt.number();

    // No more elements
    if (objIt.more())
        return BAD_VALUE("Only 2 fields allowed for circular region, but more were provided");

    out->cap = S2Cap::FromAxisAngle(centerPoint, S1Angle::Radians(radius));
    out->circle.radius = radius;
    out->circle.center = p;
    out->crs = SPHERE;
    return Status::OK();
}

//  { "type": "GeometryCollection",
//    "geometries": [
//      { "type": "Point",
//        "coordinates": [100.0, 0.0]
//      },
//      { "type": "LineString",
//        "coordinates": [ [101.0, 0.0], [102.0, 1.0] ]
//      }
//    ]
//  }
Status GeoParser::parseGeometryCollection(const BSONObj& obj,
                                          bool skipValidation,
                                          GeometryCollection* out) {
    BSONElement coordElt = dps::extractElementAtDottedPath(obj, GEOJSON_GEOMETRIES);
    if (BSONType::array != coordElt.type()) {
        return BAD_VALUE("GeometryCollection geometries must be an array, instead got type "
                         << typeName(coordElt.type()));
    }
    const vector<BSONElement>& geometries = coordElt.Array();
    if (0 == geometries.size())
        return BAD_VALUE("GeometryCollection geometries must have at least 1 element");

    for (size_t i = 0; i < geometries.size(); ++i) {
        if (BSONType::object != geometries[i].type())
            return BAD_VALUE("Element " << i
                                        << " of \"geometries\" must be an object, instead got type "
                                        << typeName(geometries[i].type()) << ": "
                                        << geometries[i].toString(false));

        const BSONObj& geoObj = geometries[i].Obj();
        GeoJSONType type = parseGeoJSONType(geoObj);

        if (GEOJSON_UNKNOWN == type)
            return BAD_VALUE("Unknown GeoJSON type: " << geometries[i].toString(false));

        if (GEOJSON_GEOMETRY_COLLECTION == type)
            return BAD_VALUE(
                "GeometryCollections cannot be nested: " << geometries[i].toString(false));

        Status status = Status::OK();
        if (GEOJSON_POINT == type) {
            out->points.resize(out->points.size() + 1);
            status = parseGeoJSONPoint(geoObj, &out->points.back());
        } else if (GEOJSON_LINESTRING == type) {
            out->lines.push_back(std::make_unique<LineWithCRS>());
            status = parseGeoJSONLine(geoObj, skipValidation, out->lines.back().get());
        } else if (GEOJSON_POLYGON == type) {
            out->polygons.push_back(std::make_unique<PolygonWithCRS>());
            status = parseGeoJSONPolygon(geoObj, skipValidation, out->polygons.back().get());
        } else if (GEOJSON_MULTI_POINT == type) {
            out->multiPoints.push_back(std::make_unique<MultiPointWithCRS>());
            status = parseMultiPoint(geoObj, out->multiPoints.back().get());
        } else if (GEOJSON_MULTI_LINESTRING == type) {
            out->multiLines.push_back(std::make_unique<MultiLineWithCRS>());
            status = parseMultiLine(geoObj, skipValidation, out->multiLines.back().get());
        } else if (GEOJSON_MULTI_POLYGON == type) {
            out->multiPolygons.push_back(std::make_unique<MultiPolygonWithCRS>());
            status = parseMultiPolygon(geoObj, skipValidation, out->multiPolygons.back().get());
        } else {
            MONGO_UNREACHABLE_TASSERT(9911957);
        }

        // Check parsing result.
        if (!status.isOK())
            return status;
    }

    return Status::OK();
}

Status GeoParser::parsePointWithMaxDistance(const BSONElement& elem,
                                            PointWithCRS* out,
                                            double* maxOut) {
    BSONElement lat, lng, maxDist;
    auto status = GeoParser::parseLegacyPointWithMaxDistance(elem, lat, lng, maxDist);
    if (!status.isOK()) {
        return status;
    }
    out->oldPoint.x = lat.number();
    out->oldPoint.y = lng.number();
    out->crs = FLAT;
    *maxOut = maxDist.number();
    return Status::OK();
}

GeoParser::GeoSpecifier GeoParser::parseGeoSpecifier(const BSONElement& type) {
    if (!type.isABSONObj()) {
        return GeoParser::UNKNOWN;
    }
    StringData fieldName = type.fieldNameStringData();
    if (fieldName == kBoxField) {
        return GeoParser::BOX;
    } else if (fieldName == kCenterField) {
        return GeoParser::CENTER;
    } else if (fieldName == kPolygonField) {
        return GeoParser::POLYGON;
    } else if (fieldName == kCenterSphereField) {
        return GeoParser::CENTER_SPHERE;
    } else if (fieldName == kGeometryField) {
        return GeoParser::GEOMETRY;
    }
    return GeoParser::UNKNOWN;
}

GeoParser::GeoJSONType GeoParser::parseGeoJSONType(const BSONObj& obj) {
    BSONElement type = dps::extractElementAtDottedPath(obj, GEOJSON_TYPE);
    if (BSONType::string != type.type()) {
        return GeoParser::GEOJSON_UNKNOWN;
    }
    return geoJSONTypeStringToEnum(type.checkAndGetStringData());
}

// TODO: SERVER-86141 audit if this method is needed else remove.
void GeoParser::assertValidGeoJSONType(const BSONObj& obj) {
    BSONElement type = dps::extractElementAtDottedPath(obj, GEOJSON_TYPE);
    uassert(8459801,
            str::stream() << "Expected valid geojson of type string, got non-string type of value "
                          << type,
            BSONType::string == type.type());
    auto str = type.checkAndGetStringData();
    uassert(8459800,
            str::stream() << "Expected valid geojson type, got " << str,
            geoJSONTypeStringToEnum(str) != GeoParser::GEOJSON_UNKNOWN);
}

GeoParser::GeoJSONType GeoParser::geoJSONTypeStringToEnum(StringData type) {
    if (GEOJSON_TYPE_POINT == type) {
        return GeoParser::GEOJSON_POINT;
    } else if (GEOJSON_TYPE_LINESTRING == type) {
        return GeoParser::GEOJSON_LINESTRING;
    } else if (GEOJSON_TYPE_POLYGON == type) {
        return GeoParser::GEOJSON_POLYGON;
    } else if (GEOJSON_TYPE_MULTI_POINT == type) {
        return GeoParser::GEOJSON_MULTI_POINT;
    } else if (GEOJSON_TYPE_MULTI_LINESTRING == type) {
        return GeoParser::GEOJSON_MULTI_LINESTRING;
    } else if (GEOJSON_TYPE_MULTI_POLYGON == type) {
        return GeoParser::GEOJSON_MULTI_POLYGON;
    } else if (GEOJSON_TYPE_GEOMETRY_COLLECTION == type) {
        return GeoParser::GEOJSON_GEOMETRY_COLLECTION;
    }
    return GeoParser::GEOJSON_UNKNOWN;
}

StringData GeoParser::geoJSONTypeEnumToString(GeoParser::GeoJSONType type) {
    switch (type) {
        case GEOJSON_UNKNOWN:
            return "unknown"_sd;
        case GEOJSON_POINT:
            return GEOJSON_TYPE_POINT;
        case GEOJSON_LINESTRING:
            return GEOJSON_TYPE_LINESTRING;
        case GEOJSON_POLYGON:
            return GEOJSON_TYPE_POLYGON;
        case GEOJSON_MULTI_POINT:
            return GEOJSON_TYPE_MULTI_POINT;
        case GEOJSON_MULTI_LINESTRING:
            return GEOJSON_TYPE_MULTI_LINESTRING;
        case GEOJSON_MULTI_POLYGON:
            return GEOJSON_TYPE_MULTI_POLYGON;
        case GEOJSON_GEOMETRY_COLLECTION:
            return GEOJSON_TYPE_GEOMETRY_COLLECTION;
    }
    MONGO_UNREACHABLE_TASSERT(8459802);
}

}  // namespace mongo
