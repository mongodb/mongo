/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kGeo

#include "mongo/db/geo/geoparser.h"

#include <cmath>
#include <string>
#include <vector>

#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "third_party/s2/s2polygonbuilder.h"

#define BAD_VALUE(error) Status(ErrorCodes::BadValue, ::mongoutils::str::stream() << error)

namespace mongo {

using std::unique_ptr;
using std::stringstream;

namespace dps = ::mongo::dotted_path_support;

// This field must be present, and...
static const string GEOJSON_TYPE = "type";
// Have one of these values:
static const string GEOJSON_TYPE_POINT = "Point";
static const string GEOJSON_TYPE_LINESTRING = "LineString";
static const string GEOJSON_TYPE_POLYGON = "Polygon";
static const string GEOJSON_TYPE_MULTI_POINT = "MultiPoint";
static const string GEOJSON_TYPE_MULTI_LINESTRING = "MultiLineString";
static const string GEOJSON_TYPE_MULTI_POLYGON = "MultiPolygon";
static const string GEOJSON_TYPE_GEOMETRY_COLLECTION = "GeometryCollection";
// This field must also be present.  The value depends on the type.
static const string GEOJSON_COORDINATES = "coordinates";
static const string GEOJSON_GEOMETRIES = "geometries";

// Coordinate System Reference
// see http://portal.opengeospatial.org/files/?artifact_id=24045
// and http://spatialreference.org/ref/epsg/4326/
// and http://www.geojson.org/geojson-spec.html#named-crs
static const string CRS_CRS84 = "urn:ogc:def:crs:OGC:1.3:CRS84";
static const string CRS_EPSG_4326 = "EPSG:4326";
static const string CRS_STRICT_WINDING = "urn:x-mongodb:crs:strictwinding:EPSG:4326";

static Status parseFlatPoint(const BSONElement& elem, Point* out, bool allowAddlFields = false) {
    if (!elem.isABSONObj())
        return BAD_VALUE("Point must be an array or object");
    BSONObjIterator it(elem.Obj());
    BSONElement x = it.next();
    if (!x.isNumber()) {
        return BAD_VALUE("Point must only contain numeric elements");
    }
    BSONElement y = it.next();
    if (!y.isNumber()) {
        return BAD_VALUE("Point must only contain numeric elements");
    }
    if (!allowAddlFields && it.more()) {
        return BAD_VALUE("Point must only contain two numeric elements");
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
        return BAD_VALUE("longitude/latitude is out of bounds, lng: " << lng << " lat: " << lat);
    // Note that it's (lat, lng) for S2 but (lng, lat) for MongoDB.
    S2LatLng ll = S2LatLng::FromDegrees(lat, lng).Normalized();
    // This shouldn't happen since we should only have valid lng/lats.
    if (!ll.is_valid()) {
        stringstream ss;
        ss << "coords invalid after normalization, lng = " << lng << " lat = " << lat << endl;
        uasserted(17125, ss.str());
    }
    *out = ll.ToPoint();
    return Status::OK();
}

static Status parseGeoJSONCoordinate(const BSONElement& elem, S2Point* out) {
    if (Array != elem.type()) {
        return BAD_VALUE("GeoJSON coordinates must be an array");
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
    if (Array != elem.type()) {
        return BAD_VALUE("GeoJSON coordinates must be an array of coordinates");
    }
    BSONObjIterator it(elem.Obj());
    // Iterate all coordinates in array
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
    for (size_t i = 1; i < vertices->size(); ++i) {
        if ((*vertices)[i - 1] == (*vertices)[i]) {
            vertices->erase(vertices->begin() + i);
            // We could have > 2 adjacent identical vertices, and must examine i again.
            --i;
        }
    }
}

static Status isLoopClosed(const vector<S2Point>& loop, const BSONElement loopElt) {
    if (loop.empty()) {
        return BAD_VALUE("Loop has no vertices: " << loopElt.toString(false));
    }

    if (loop[0] != loop[loop.size() - 1]) {
        return BAD_VALUE("Loop is not closed: " << loopElt.toString(false));
    }

    return Status::OK();
}

static Status parseGeoJSONPolygonCoordinates(const BSONElement& elem,
                                             bool skipValidation,
                                             S2Polygon* out) {
    if (Array != elem.type()) {
        return BAD_VALUE("Polygon coordinates must be an array");
    }

    OwnedPointerVector<S2Loop> loops;
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
            return BAD_VALUE(
                "Loop must have at least 3 different vertices: " << coordinateElt.toString(false));
        }

        S2Loop* loop = new S2Loop(points);
        loops.push_back(loop);

        // Check whether this loop is valid.
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
        if (loops.size() > 1 && !loops[0]->Contains(loop)) {
            return BAD_VALUE(
                "Secondary loops not contained by first exterior loop - "
                "secondary loops must be holes: "
                << coordinateElt.toString(false)
                << " first loop: "
                << elem.Obj().firstElement().toString(false));
        }
    }

    if (loops.empty()) {
        return BAD_VALUE("Polygon has no loops.");
    }

    // Check if the given loops form a valid polygon.
    // 1. If a loop contains an edge AB, then no other loop may contain AB or BA.
    // 2. No loop covers more than half of the sphere.
    // 3. No two loops cross.
    if (!skipValidation && !S2Polygon::IsValid(loops.vector(), &err))
        return BAD_VALUE("Polygon isn't valid: " << err << " " << elem.toString(false));

    // Given all loops are valid / normalized and S2Polygon::IsValid() above returns true.
    // The polygon must be valid. See S2Polygon member function IsValid().

    // Transfer ownership of the loops and clears loop vector.
    out->Init(&loops.mutableVector());

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
    if (Array != elem.type())
        return BAD_VALUE("Coordinates of polygon must be an array");


    const vector<BSONElement>& coordinates = elem.Array();
    // Only one loop is allowed in a BigSimplePolygon
    if (coordinates.size() != 1) {
        return BAD_VALUE(
            "Only one simple loop is allowed in a big polygon: " << elem.toString(false));
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
        return BAD_VALUE("Loop must have at least 3 different vertices: " << elem.toString(false));
    }

    unique_ptr<S2Loop> loop(new S2Loop(exteriorVertices));
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

    BSONElement crsElt = obj["crs"];
    // "crs" field doesn't exist, return the default SPHERE
    if (crsElt.eoo()) {
        return Status::OK();
    }

    if (!crsElt.isABSONObj())
        return BAD_VALUE("GeoJSON CRS must be an object");
    BSONObj crsObj = crsElt.embeddedObject();

    // "type": "name"
    if (String != crsObj["type"].type() || "name" != crsObj["type"].String())
        return BAD_VALUE("GeoJSON CRS must have field \"type\": \"name\"");

    // "properties"
    BSONElement propertiesElt = crsObj["properties"];
    if (!propertiesElt.isABSONObj())
        return BAD_VALUE("CRS must have field \"properties\" which is an object");
    BSONObj propertiesObj = propertiesElt.embeddedObject();
    if (String != propertiesObj["name"].type())
        return BAD_VALUE("In CRS, \"properties.name\" must be a string");
    const string& name = propertiesObj["name"].String();
    if (CRS_CRS84 == name || CRS_EPSG_4326 == name) {
        *crs = SPHERE;
    } else if (CRS_STRICT_WINDING == name) {
        if (!allowStrictSphere) {
            return BAD_VALUE("Strict winding order is only supported by polygon");
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
            return BAD_VALUE(
                "GeoJSON LineString must have at least 2 vertices: " << elem.toString(false));

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
    if (!elem.isABSONObj())
        return BAD_VALUE("Point must be an array or object");

    BSONObj obj = elem.Obj();
    // location: [1, 2] or location: {x: 1, y:2}
    if (Array == elem.type() || obj.firstElement().isNumber()) {
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
        return BAD_VALUE("Polygon must have at least 3 points");
    out->oldPolygon.init(points);
    out->crs = FLAT;
    return Status::OK();
}

// { "type": "Point", "coordinates": [100.0, 0.0] }
Status GeoParser::parseGeoJSONPoint(const BSONObj& obj, PointWithCRS* out) {
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
        return BAD_VALUE("longitude/latitude is out of bounds, lng: " << out->oldPoint.x << " lat: "
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
    BSONElement coordElt = dps::extractElementAtPath(obj, GEOJSON_COORDINATES);
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

    BSONElement coordElt = dps::extractElementAtPath(obj, GEOJSON_COORDINATES);
    if (Array != coordElt.type())
        return BAD_VALUE("MultiLineString coordinates must be an array");

    out->lines.clear();
    vector<S2Polyline*>& lines = out->lines.mutableVector();

    BSONObjIterator it(coordElt.Obj());

    // Iterate array
    while (it.more()) {
        lines.push_back(new S2Polyline());
        status = parseGeoJSONLineCoordinates(it.next(), skipValidation, lines.back());
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

    BSONElement coordElt = dps::extractElementAtPath(obj, GEOJSON_COORDINATES);
    if (Array != coordElt.type())
        return BAD_VALUE("MultiPolygon coordinates must be an array");

    out->polygons.clear();
    vector<S2Polygon*>& polygons = out->polygons.mutableVector();

    BSONObjIterator it(coordElt.Obj());
    // Iterate array
    while (it.more()) {
        polygons.push_back(new S2Polygon());
        status = parseGeoJSONPolygonCoordinates(it.next(), skipValidation, polygons.back());
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
        return BAD_VALUE("radius must be a non-negative number");

    // No more
    if (objIt.more())
        return BAD_VALUE("Only 2 fields allowed for circular region");

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
    if (!radiusElt.isNumber() || !(radiusElt.number() >= 0))
        return BAD_VALUE("radius must be a non-negative number");
    double radius = radiusElt.number();

    // No more elements
    if (objIt.more())
        return BAD_VALUE("Only 2 fields allowed for circular region");

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
    BSONElement coordElt = dps::extractElementAtPath(obj, GEOJSON_GEOMETRIES);
    if (Array != coordElt.type())
        return BAD_VALUE("GeometryCollection geometries must be an array");

    const vector<BSONElement>& geometries = coordElt.Array();
    if (0 == geometries.size())
        return BAD_VALUE("GeometryCollection geometries must have at least 1 element");

    for (size_t i = 0; i < geometries.size(); ++i) {
        if (Object != geometries[i].type())
            return BAD_VALUE("Element " << i << " of \"geometries\" is not an object");

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
            out->lines.mutableVector().push_back(new LineWithCRS());
            status = parseGeoJSONLine(geoObj, skipValidation, out->lines.vector().back());
        } else if (GEOJSON_POLYGON == type) {
            out->polygons.mutableVector().push_back(new PolygonWithCRS());
            status = parseGeoJSONPolygon(geoObj, skipValidation, out->polygons.vector().back());
        } else if (GEOJSON_MULTI_POINT == type) {
            out->multiPoints.mutableVector().push_back(new MultiPointWithCRS());
            status = parseMultiPoint(geoObj, out->multiPoints.mutableVector().back());
        } else if (GEOJSON_MULTI_LINESTRING == type) {
            out->multiLines.mutableVector().push_back(new MultiLineWithCRS());
            status = parseMultiLine(geoObj, skipValidation, out->multiLines.mutableVector().back());
        } else if (GEOJSON_MULTI_POLYGON == type) {
            out->multiPolygons.mutableVector().push_back(new MultiPolygonWithCRS());
            status = parseMultiPolygon(
                geoObj, skipValidation, out->multiPolygons.mutableVector().back());
        } else {
            // Should not reach here.
            invariant(false);
        }

        // Check parsing result.
        if (!status.isOK())
            return status;
    }

    return Status::OK();
}

bool GeoParser::parsePointWithMaxDistance(const BSONObj& obj, PointWithCRS* out, double* maxOut) {
    BSONObjIterator it(obj);
    if (!it.more()) {
        return false;
    }

    BSONElement lng = it.next();
    if (!lng.isNumber()) {
        return false;
    }
    if (!it.more()) {
        return false;
    }

    BSONElement lat = it.next();
    if (!lat.isNumber()) {
        return false;
    }
    if (!it.more()) {
        return false;
    }

    BSONElement dist = it.next();
    if (!dist.isNumber()) {
        return false;
    }
    if (it.more()) {
        return false;
    }

    out->oldPoint.x = lng.number();
    out->oldPoint.y = lat.number();
    out->crs = FLAT;
    *maxOut = dist.number();
    return true;
}

GeoParser::GeoSpecifier GeoParser::parseGeoSpecifier(const BSONElement& type) {
    if (!type.isABSONObj()) {
        return GeoParser::UNKNOWN;
    }
    const char* fieldName = type.fieldName();
    if (mongoutils::str::equals(fieldName, "$box")) {
        return GeoParser::BOX;
    } else if (mongoutils::str::equals(fieldName, "$center")) {
        return GeoParser::CENTER;
    } else if (mongoutils::str::equals(fieldName, "$polygon")) {
        return GeoParser::POLYGON;
    } else if (mongoutils::str::equals(fieldName, "$centerSphere")) {
        return GeoParser::CENTER_SPHERE;
    } else if (mongoutils::str::equals(fieldName, "$geometry")) {
        return GeoParser::GEOMETRY;
    }
    return GeoParser::UNKNOWN;
}

GeoParser::GeoJSONType GeoParser::parseGeoJSONType(const BSONObj& obj) {
    BSONElement type = dps::extractElementAtPath(obj, GEOJSON_TYPE);
    if (String != type.type()) {
        return GeoParser::GEOJSON_UNKNOWN;
    }
    const string& typeString = type.String();
    if (GEOJSON_TYPE_POINT == typeString) {
        return GeoParser::GEOJSON_POINT;
    } else if (GEOJSON_TYPE_LINESTRING == typeString) {
        return GeoParser::GEOJSON_LINESTRING;
    } else if (GEOJSON_TYPE_POLYGON == typeString) {
        return GeoParser::GEOJSON_POLYGON;
    } else if (GEOJSON_TYPE_MULTI_POINT == typeString) {
        return GeoParser::GEOJSON_MULTI_POINT;
    } else if (GEOJSON_TYPE_MULTI_LINESTRING == typeString) {
        return GeoParser::GEOJSON_MULTI_LINESTRING;
    } else if (GEOJSON_TYPE_MULTI_POLYGON == typeString) {
        return GeoParser::GEOJSON_MULTI_POLYGON;
    } else if (GEOJSON_TYPE_GEOMETRY_COLLECTION == typeString) {
        return GeoParser::GEOJSON_GEOMETRY_COLLECTION;
    }
    return GeoParser::GEOJSON_UNKNOWN;
}

}  // namespace mongo
