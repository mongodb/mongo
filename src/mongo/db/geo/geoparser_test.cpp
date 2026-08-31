// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * This file contains tests for mongo/db/geo/geoparser.cpp.
 */

#include "mongo/db/geo/geoparser.h"

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/geo/geometry_container.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <s2cellid.h>
#include <s2latlng.h>
#include <s2polygon.h>

// Wrap a BSON object to a BSON element.
#define BSON_ELT(bson) BSON("" << (bson)).firstElement()

using namespace mongo;

namespace {

// Returns true if (lng1, lat) and (lng2, lat) collapse to the same S2Point.
bool collapsesOnCurrentGlibc(double lat, double lng1, double lng2) {
    return S2LatLng::FromDegrees(lat, lng1).Normalized().ToPoint() ==
        S2LatLng::FromDegrees(lat, lng2).Normalized().ToPoint();
}

// Asserts that 'polygon' consists of a single ring holding exactly 'expectedVertexCount' vertices.
// Used by the recovery tests to pin down how many vertices survived, not merely that parsing
// succeeded.
void assertSingleLoopVertexCount(const PolygonWithCRS& polygon, int expectedVertexCount) {
    ASSERT(polygon.s2Polygon);
    ASSERT_EQUALS(1, polygon.s2Polygon->num_loops());
    ASSERT_EQUALS(expectedVertexCount, polygon.s2Polygon->loop(0)->num_vertices());
}

TEST(GeoParser, parseGeoSpecifier) {
    ASSERT_EQUALS(
        GeoParser::parseGeoSpecifier(fromjson("{$box : [[1, 2], [3, 4]]}").firstElement()),
        GeoParser::BOX);
    ASSERT_EQUALS(GeoParser::parseGeoSpecifier(fromjson("{$center : [[0, 0], 4]}").firstElement()),
                  GeoParser::CENTER);
    ASSERT_EQUALS(
        GeoParser::parseGeoSpecifier(fromjson("{$centerSphere : [[0, 0], 1]}").firstElement()),
        GeoParser::CENTER_SPHERE);
    ASSERT_EQUALS(
        GeoParser::parseGeoSpecifier(
            fromjson("{$geometry : {'type':'Point', 'coordinates': [40, 5]}}").firstElement()),
        GeoParser::GEOMETRY);
}

TEST(GeoParser, parseGeoJSONPoint) {
    PointWithCRS point;

    ASSERT_OK(
        GeoParser::parseGeoJSONPoint(fromjson("{'type':'Point', 'coordinates': [40, 5]}"), &point));
    ASSERT_OK(GeoParser::parseGeoJSONPoint(
        fromjson("{'type':'Point', 'coordinates': [-40.3, -5.0]}"), &point));
    ASSERT_NOT_OK(
        GeoParser::parseGeoJSONPoint(fromjson("{'type':'Point', 'coordhats': [40, -5]}"), &point));
    ASSERT_NOT_OK(
        GeoParser::parseGeoJSONPoint(fromjson("{'type':'Point', 'coordinates': 40}"), &point));
    ASSERT_OK(GeoParser::parseGeoJSONPoint(fromjson("{'type':'Point', 'coordinates': [40, -5, 7]}"),
                                           &point));

    // Make sure lat is in range
    ASSERT_OK(GeoParser::parseGeoJSONPoint(fromjson("{'type':'Point', 'coordinates': [0, 90.0]}"),
                                           &point));
    ASSERT_OK(GeoParser::parseGeoJSONPoint(fromjson("{'type':'Point', 'coordinates': [0, -90.0]}"),
                                           &point));
    ASSERT_OK(GeoParser::parseGeoJSONPoint(fromjson("{'type':'Point', 'coordinates': [180, 90.0]}"),
                                           &point));
    ASSERT_OK(GeoParser::parseGeoJSONPoint(
        fromjson("{'type':'Point', 'coordinates': [-180, -90.0]}"), &point));
    ASSERT_NOT_OK(GeoParser::parseGeoJSONPoint(
        fromjson("{'type':'Point', 'coordinates': [180.01, 90.0]}"), &point));
    ASSERT_NOT_OK(GeoParser::parseGeoJSONPoint(
        fromjson("{'type':'Point', 'coordinates': [-180.01, -90.0]}"), &point));
    ASSERT_NOT_OK(GeoParser::parseGeoJSONPoint(
        fromjson("{'type':'Point', 'coordinates': [0, 90.1]}"), &point));
    ASSERT_NOT_OK(GeoParser::parseGeoJSONPoint(
        fromjson("{'type':'Point', 'coordinates': [0, -90.1]}"), &point));
}

TEST(GeoParser, parseGeoJSONLine) {
    LineWithCRS polyline;

    ASSERT_OK(GeoParser::parseGeoJSONLine(
        fromjson("{'type':'LineString', 'coordinates':[[1,2], [3,4]]}"), false, &polyline));
    ASSERT_OK(GeoParser::parseGeoJSONLine(
        fromjson("{'type':'LineString', 'coordinates':[[0,-90], [0,90]]}"), false, &polyline));
    ASSERT_OK(GeoParser::parseGeoJSONLine(
        fromjson("{'type':'LineString', 'coordinates':[[180,-90], [-180,90]]}"), false, &polyline));
    ASSERT_NOT_OK(GeoParser::parseGeoJSONLine(
        fromjson("{'type':'LineString', 'coordinates':[[180.1,-90], [-180.1,90]]}"),
        false,
        &polyline));
    ASSERT_NOT_OK(GeoParser::parseGeoJSONLine(
        fromjson("{'type':'LineString', 'coordinates':[[0,-91], [0,90]]}"), false, &polyline));
    ASSERT_NOT_OK(GeoParser::parseGeoJSONLine(
        fromjson("{'type':'LineString', 'coordinates':[[0,-90], [0,91]]}"), false, &polyline));
    ASSERT_OK(GeoParser::parseGeoJSONLine(
        fromjson("{'type':'LineString', 'coordinates':[[1,2], [3,4], [5,6]]}"), false, &polyline));
    ASSERT_NOT_OK(GeoParser::parseGeoJSONLine(
        fromjson("{'type':'LineString', 'coordinates':[[1,2]]}"), false, &polyline));
    ASSERT_NOT_OK(GeoParser::parseGeoJSONLine(
        fromjson("{'type':'LineString', 'coordinates':[['chicken','little']]}"), false, &polyline));
    ASSERT_NOT_OK(GeoParser::parseGeoJSONLine(
        fromjson("{'type':'LineString', 'coordinates':[1,2, 3, 4]}"), false, &polyline));
    ASSERT_OK(GeoParser::parseGeoJSONLine(
        fromjson("{'type':'LineString', 'coordinates':[[1,2, 3], [3,4, 5], [5,6]]}"),
        false,
        &polyline));
    ASSERT_NOT_OK(GeoParser::parseGeoJSONLine(
        fromjson("{'type':'LineString', 'coordinates':[[1,2], [1,2]]}"), false, &polyline));
}

TEST(GeoParser, parseGeoJSONPolygon) {
    PolygonWithCRS polygon;

    ASSERT_OK(GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]] ]}"),
        false,
        &polygon));
    // No out of bounds points
    ASSERT_NOT_OK(GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,91],[0,5],[0,0]] ]}"),
        false,
        &polygon));
    ASSERT_OK(GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[180,0],[5,5],[0,5],[0,0]] ]}"),
        false,
        &polygon));
    ASSERT_NOT_OK(GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[181,0],[5,5],[0,5],[0,0]] ]}"),
        false,
        &polygon));
    // And one with a hole.
    ASSERT_OK(GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]],"
                 " [[1,1],[4,1],[4,4],[1,4],[1,1]] ]}"),
        false,
        &polygon));
    // Latitudes must be OK
    ASSERT_NOT_OK(GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,91],[0,91],[0,0]],"
                 " [[1,1],[4,1],[4,4],[1,4],[1,1]] ]}"),
        false,
        &polygon));
    // First point must be the same as the last.
    ASSERT_NOT_OK(GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon', 'coordinates':[ [[1,2],[3,4],[5,6]] ]}"), false, &polygon));
    // Extra elements are allowed
    ASSERT_OK(GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon', 'coordinates':[ [[0,0,0,0],[5,0,0],[5,5,1],"
                 " [0,5],[0,0]] ]}"),
        false,
        &polygon));

    // Test functionality of polygon
    PointWithCRS point;
    ASSERT_OK(
        GeoParser::parseGeoJSONPoint(fromjson("{'type':'Point', 'coordinates': [2, 2]}"), &point));

    PolygonWithCRS polygonA;
    ASSERT_OK(GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]] ]}"),
        false,
        &polygonA));
    ASSERT_TRUE(polygonA.s2Polygon->Contains(point.point));

    PolygonWithCRS polygonB;
    ASSERT_OK(GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]],"
                 " [[1,1],[1,4],[4,4],[4,1],[1,1]] ]}"),
        false,
        &polygonB));
    // We removed this in the hole.
    ASSERT_FALSE(polygonB.s2Polygon->Contains(point.point));

    // Now we reverse the orientations and verify that the code fixes it up
    // (outer loop must be CCW, inner CW).
    PolygonWithCRS polygonC;
    ASSERT_OK(GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[0,5],[5,5],[5,0],[0,0]] ]}"),
        false,
        &polygonC));
    ASSERT_TRUE(polygonC.s2Polygon->Contains(point.point));

    PolygonWithCRS polygonD;
    ASSERT_OK(GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[0,5],[5,5],[5,0],[0,0]],"
                 " [[1,1],[1,4],[4,4],[4,1],[1,1]] ]}"),
        false,
        &polygonD));
    // Also removed in the loop.
    ASSERT_FALSE(polygonD.s2Polygon->Contains(point.point));

    //
    // Bad polygon examples
    //

    // Polygon with not enough points, because some are duplicated
    PolygonWithCRS polygonBad;
    ASSERT_NOT_OK(GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon', 'coordinates':[[ [0,0], [0,0], [5,5], [5,5], [0,0] ]]}"),
        false,
        &polygonBad));
}

TEST(GeoParser, parseGeoJSONPolygonStrictSphere) {
    string crs = "crs:{ type: 'name', properties:{name:'" + std::string{CRS_STRICT_WINDING} + "'}}";
    PolygonWithCRS polygon;
    BSONObj bigSimplePolygon = fromjson(
        "{'type':'Polygon', 'coordinates':[ "
        "[[0,0],[5,0],[5,5],[0,5],[0,0]]], " +
        crs + "}");
    ASSERT_OK(GeoParser::parseGeoJSONPolygon(bigSimplePolygon, false, &polygon));

    BSONObj bigSimplePolygonWithDuplicates = fromjson(
        "{'type':'Polygon', 'coordinates':[ "
        "[[0,0],[5,0],[5,0],[0,0],[0,0]]], " +
        crs + "}");
    ASSERT_NOT_OK(GeoParser::parseGeoJSONPolygon(bigSimplePolygonWithDuplicates, false, &polygon));

    BSONObj bigSimplePolygonWithFewPoints = fromjson(
        "{'type':'Polygon', 'coordinates':[ "
        "[[0,0]]], " +
        crs + "}");
    ASSERT_NOT_OK(GeoParser::parseGeoJSONPolygon(bigSimplePolygonWithFewPoints, false, &polygon));
}

TEST(GeoParser, parseGeoJSONCRS) {
    string goodCRS1 = "crs:{ type: 'name', properties:{name:'EPSG:4326'}}";
    string goodCRS2 = "crs:{ type: 'name', properties:{name:'urn:ogc:def:crs:OGC:1.3:CRS84'}}";
    string badCRS1 = "crs:{ type: 'name', properties:{name:'EPSG:2000'}}";
    string badCRS2 = "crs:{ type: 'name', properties:{name:'urn:ogc:def:crs:OGC:1.3:CRS83'}}";

    BSONObj point1 = fromjson("{'type':'Point', 'coordinates': [40, 5], " + goodCRS1 + "}");
    BSONObj point2 = fromjson("{'type':'Point', 'coordinates': [40, 5], " + goodCRS2 + "}");
    PointWithCRS point;
    ASSERT_OK(GeoParser::parseGeoJSONPoint(point1, &point));
    ASSERT_OK(GeoParser::parseGeoJSONPoint(point2, &point));
    BSONObj point3 = fromjson("{'type':'Point', 'coordinates': [40, 5], " + badCRS1 + "}");
    BSONObj point4 = fromjson("{'type':'Point', 'coordinates': [40, 5], " + badCRS2 + "}");
    ASSERT_NOT_OK(GeoParser::parseGeoJSONPoint(point3, &point));
    ASSERT_NOT_OK(GeoParser::parseGeoJSONPoint(point4, &point));

    PolygonWithCRS polygon;
    BSONObj polygon1 = fromjson(
        "{'type':'Polygon', 'coordinates':[ "
        "[[0,0],[5,0],[5,5],[0,5],[0,0]],"
        " [[1,1],[1,4],[4,4],[4,1],[1,1]] ]," +
        goodCRS1 + "}");
    ASSERT_OK(GeoParser::parseGeoJSONPolygon(polygon1, false, &polygon));
    BSONObj polygon2 = fromjson(
        "{'type':'Polygon', 'coordinates':[ "
        "[[0,0],[5,0],[5,5],[0,5],[0,0]],"
        " [[1,1],[1,4],[4,4],[4,1],[1,1]] ]," +
        badCRS2 + "}");
    ASSERT_NOT_OK(GeoParser::parseGeoJSONPolygon(polygon2, false, &polygon));

    LineWithCRS line;
    BSONObj line1 =
        fromjson("{'type':'LineString', 'coordinates':[[1,2], [3,4], [5,6]]," + goodCRS2 + "}");
    ASSERT_OK(GeoParser::parseGeoJSONLine(line1, false, &line));
    BSONObj line2 =
        fromjson("{'type':'LineString', 'coordinates':[[1,2], [3,4], [5,6]]," + badCRS1 + "}");
    ASSERT_NOT_OK(GeoParser::parseGeoJSONLine(line2, false, &line));
}

TEST(GeoParser, parseLegacyPoint) {
    PointWithCRS point;
    ASSERT_OK(GeoParser::parseLegacyPoint(BSON_ELT(BSON_ARRAY(0 << 1)), &point));
    ASSERT_NOT_OK(GeoParser::parseLegacyPoint(BSON_ELT(BSON_ARRAY(0)), &point));
    ASSERT_NOT_OK(GeoParser::parseLegacyPoint(BSON_ELT(BSON_ARRAY(0 << 1 << 2)), &point));
    ASSERT_OK(GeoParser::parseLegacyPoint(BSON_ELT(fromjson("{x: 50, y:40}")), &point));
    ASSERT_NOT_OK(GeoParser::parseLegacyPoint(BSON_ELT(fromjson("{x: '50', y:40}")), &point));
    ASSERT_NOT_OK(GeoParser::parseLegacyPoint(BSON_ELT(fromjson("{x: 5, y:40, z:50}")), &point));
    ASSERT_NOT_OK(GeoParser::parseLegacyPoint(BSON_ELT(fromjson("{x: 5}")), &point));
}

TEST(GeoParser, parsePointWithMaxDistance) {
    PointWithCRS point;
    double maxDistance;
    ASSERT_NOT_OK(GeoParser::parsePointWithMaxDistance(BSON_ELT("hi"), &point, &maxDistance));
    ASSERT_NOT_OK(
        GeoParser::parsePointWithMaxDistance(BSON_ELT(BSON_ARRAY(0)), &point, &maxDistance));
    ASSERT_NOT_OK(
        GeoParser::parsePointWithMaxDistance(BSON_ELT(BSON_ARRAY(0 << 1)), &point, &maxDistance));
    ASSERT_OK(GeoParser::parsePointWithMaxDistance(
        BSON_ELT(BSON_ARRAY(0 << 1 << 2)), &point, &maxDistance));
    ASSERT_NOT_OK(GeoParser::parsePointWithMaxDistance(
        BSON_ELT(BSON_ARRAY(0 << 1 << 2 << 3)), &point, &maxDistance));
    ASSERT_NOT_OK(GeoParser::parsePointWithMaxDistance(
        BSON_ELT(BSON_ARRAY(0 << "foo" << 2)), &point, &maxDistance));
    ASSERT_NOT_OK(
        GeoParser::parsePointWithMaxDistance(BSON_ELT(fromjson("{x: 5}")), &point, &maxDistance));
    ASSERT_NOT_OK(GeoParser::parsePointWithMaxDistance(
        BSON_ELT(fromjson("{x: 50, y:40}")), &point, &maxDistance));
    ASSERT_OK(GeoParser::parsePointWithMaxDistance(
        BSON_ELT(fromjson("{x: 5, y:40, z:50}")), &point, &maxDistance));
    ASSERT_NOT_OK(GeoParser::parsePointWithMaxDistance(
        BSON_ELT(fromjson("{x: 5, y:40, z:50, a: 100}")), &point, &maxDistance));
    ASSERT_NOT_OK(GeoParser::parsePointWithMaxDistance(
        BSON_ELT(fromjson("{x: 5, y: 'foo' , z:50}")), &point, &maxDistance));
}

TEST(GeoParser, parseLegacyPolygon) {
    PolygonWithCRS polygon;

    // Parse the object after field name "$polygon"
    ASSERT_OK(
        GeoParser::parseLegacyPolygon(fromjson("[[10,20],[10,40],[30,40],[30,20]]"), &polygon));
    ASSERT(polygon.crs == FLAT);

    ASSERT_OK(GeoParser::parseLegacyPolygon(fromjson("[[10,20], [10,40], [30,40]]"), &polygon));
    ASSERT(polygon.crs == FLAT);

    ASSERT_NOT_OK(GeoParser::parseLegacyPolygon(fromjson("[[10,20],[10,40]]"), &polygon));
    ASSERT_NOT_OK(
        GeoParser::parseLegacyPolygon(fromjson("[['10',20],[10,40],[30,40],[30,20]]"), &polygon));
    ASSERT_NOT_OK(
        GeoParser::parseLegacyPolygon(fromjson("[[10,20,30],[10,40],[30,40],[30,20]]"), &polygon));
    ASSERT_OK(GeoParser::parseLegacyPolygon(
        fromjson("{a:{x:40,y:5},b:{x:40,y:6},c:{x:41,y:6},d:{x:41,y:5}}"), &polygon));
}

TEST(GeoParser, parseMultiPoint) {
    mongo::MultiPointWithCRS mp;

    ASSERT_OK(GeoParser::parseMultiPoint(
        fromjson("{'type':'MultiPoint','coordinates':[[1,2],[3,4]]}"), &mp));
    ASSERT_EQUALS(mp.points.size(), (size_t)2);

    ASSERT_OK(
        GeoParser::parseMultiPoint(fromjson("{'type':'MultiPoint','coordinates':[[3,4]]}"), &mp));
    ASSERT_EQUALS(mp.points.size(), (size_t)1);

    ASSERT_OK(GeoParser::parseMultiPoint(
        fromjson("{'type':'MultiPoint','coordinates':[[1,2],[3,4],[5,6],[7,8]]}"), &mp));
    ASSERT_EQUALS(mp.points.size(), (size_t)4);

    ASSERT_NOT_OK(
        GeoParser::parseMultiPoint(fromjson("{'type':'MultiPoint','coordinates':[]}"), &mp));
    ASSERT_NOT_OK(GeoParser::parseMultiPoint(
        fromjson("{'type':'MultiPoint','coordinates':[[181,2],[3,4]]}"), &mp));
    ASSERT_NOT_OK(GeoParser::parseMultiPoint(
        fromjson("{'type':'MultiPoint','coordinates':[[1,-91],[3,4]]}"), &mp));
    ASSERT_NOT_OK(GeoParser::parseMultiPoint(
        fromjson("{'type':'MultiPoint','coordinates':[[181,2],[3,'chicken']]}"), &mp));
}

TEST(GeoParser, parseMultiLine) {
    mongo::MultiLineWithCRS ml;

    ASSERT_OK(GeoParser::parseMultiLine(
        fromjson("{'type':'MultiLineString','coordinates':[ [[1,1],[2,2],[3,3]],"
                 "[[4,5],[6,7]]]}"),
        false,
        &ml));
    ASSERT_EQUALS(ml.lines.size(), (size_t)2);

    ASSERT_OK(GeoParser::parseMultiLine(
        fromjson("{'type':'MultiLineString','coordinates':[ [[1,1],[2,2]],"
                 "[[4,5],[6,7]]]}"),
        false,
        &ml));
    ASSERT_EQUALS(ml.lines.size(), (size_t)2);

    ASSERT_OK(GeoParser::parseMultiLine(
        fromjson("{'type':'MultiLineString','coordinates':[ [[1,1],[2,2]]]}"), false, &ml));
    ASSERT_EQUALS(ml.lines.size(), (size_t)1);

    ASSERT_OK(GeoParser::parseMultiLine(
        fromjson("{'type':'MultiLineString','coordinates':[ [[1,1],[2,2]],"
                 "[[2,2],[1,1]]]}"),
        false,
        &ml));
    ASSERT_EQUALS(ml.lines.size(), (size_t)2);

    ASSERT_NOT_OK(GeoParser::parseMultiLine(
        fromjson("{'type':'MultiLineString','coordinates':[ [[1,1]]]}"), false, &ml));
    ASSERT_NOT_OK(GeoParser::parseMultiLine(
        fromjson("{'type':'MultiLineString','coordinates':[ [[1,1]],[[1,2],[3,4]]]}"), false, &ml));
    ASSERT_NOT_OK(GeoParser::parseMultiLine(
        fromjson("{'type':'MultiLineString','coordinates':[ [[181,1],[2,2]]]}"), false, &ml));
    ASSERT_NOT_OK(GeoParser::parseMultiLine(
        fromjson("{'type':'MultiLineString','coordinates':[ [[181,1],[2,-91]]]}"), false, &ml));
}

TEST(GeoParser, parseMultiPolygon) {
    mongo::MultiPolygonWithCRS mp;

    ASSERT_OK(GeoParser::parseMultiPolygon(
        fromjson("{'type':'MultiPolygon','coordinates':["
                 "[[[102.0, 2.0], [103.0, 2.0], [103.0, 3.0], [102.0, 3.0], [102.0, 2.0]]],"
                 "[[[100.0, 0.0], [101.0, 0.0], [101.0, 1.0], [100.0, 1.0], [100.0, 0.0]],"
                 "[[100.2, 0.2], [100.8, 0.2], [100.8, 0.8], [100.2, 0.8], [100.2, 0.2]]]"
                 "]}"),
        false,
        &mp));
    ASSERT_EQUALS(mp.polygons.size(), (size_t)2);

    ASSERT_OK(GeoParser::parseMultiPolygon(
        fromjson("{'type':'MultiPolygon','coordinates':["
                 "[[[100.0, 0.0], [101.0, 0.0], [101.0, 1.0], [100.0, 1.0], [100.0, 0.0]],"
                 "[[100.2, 0.2], [100.8, 0.2], [100.8, 0.8], [100.2, 0.8], [100.2, 0.2]]]"
                 "]}"),
        false,
        &mp));
    ASSERT_EQUALS(mp.polygons.size(), (size_t)1);
}

TEST(GeoParser, parseGeometryCollection) {
    {
        mongo::GeometryCollection gc;
        BSONObj obj = fromjson(
            "{ 'type': 'GeometryCollection', 'geometries': ["
            "{ 'type': 'Point','coordinates': [100.0,0.0]},"
            "{ 'type': 'LineString', 'coordinates': [ [101.0, 0.0], [102.0, 1.0] ]}"
            "]}");
        ASSERT_OK(GeoParser::parseGeometryCollection(obj, false, &gc));
        ASSERT_FALSE(gc.supportsContains());
    }

    {
        BSONObj obj = fromjson(
            "{ 'type': 'GeometryCollection', 'geometries': ["
            "{'type':'MultiPolygon','coordinates':["
            "[[[102.0, 2.0], [103.0, 2.0], [103.0, 3.0], [102.0, 3.0], [102.0, 2.0]]],"
            "[[[100.0, 0.0], [101.0, 0.0], [101.0, 1.0], [100.0, 1.0], [100.0, 0.0]],"
            "[[100.2, 0.2], [100.8, 0.2], [100.8, 0.8], [100.2, 0.8], [100.2, 0.2]]]"
            "]}"
            "]}");

        mongo::GeometryCollection gc;
        ASSERT_OK(GeoParser::parseGeometryCollection(obj, false, &gc));
        ASSERT_TRUE(gc.supportsContains());
    }

    {
        BSONObj obj = fromjson(
            "{ 'type': 'GeometryCollection', 'geometries': ["
            "{'type':'Polygon', 'coordinates':[ [[0,0],[0,91],[5,5],[5,0],[0,0]] ]},"
            "{'type':'MultiPolygon','coordinates':["
            "[[[102.0, 2.0], [103.0, 2.0], [103.0, 3.0], [102.0, 3.0], [102.0, 2.0]]],"
            "[[[100.0, 0.0], [101.0, 0.0], [101.0, 1.0], [100.0, 1.0], [100.0, 0.0]],"
            "[[100.2, 0.2], [100.8, 0.2], [100.8, 0.8], [100.2, 0.8], [100.2, 0.2]]]"
            "]}"
            "]}");
        mongo::GeometryCollection gc;
        ASSERT_NOT_OK(GeoParser::parseGeometryCollection(obj, false, &gc));
    }

    {
        BSONObj obj = fromjson(
            "{ 'type': 'GeometryCollection', 'geometries': ["
            "{'type':'Polygon', 'coordinates':[ [[0,0],[0,5],[5,5],[5,0],[0,0]] ]},"
            "{'type':'MultiPolygon','coordinates':["
            "[[[102.0, 2.0], [103.0, 2.0], [103.0, 3.0], [102.0, 3.0], [102.0, 2.0]]],"
            "[[[100.0, 0.0], [101.0, 0.0], [101.0, 1.0], [100.0, 1.0], [100.0, 0.0]],"
            "[[100.2, 0.2], [100.8, 0.2], [100.8, 0.8], [100.2, 0.8], [100.2, 0.2]]]"
            "]}"
            "]}");

        mongo::GeometryCollection gc;
        ASSERT_OK(GeoParser::parseGeometryCollection(obj, false, &gc));
        ASSERT_TRUE(gc.supportsContains());
    }

    // A strict-winding polygon inside a GeometryCollection parses successfully; the CRS is
    // preserved on the PolygonWithCRS so callers can detect and reject it before use.
    {
        string strictCRS =
            "crs:{ type: 'name', properties:{name:'" + std::string{CRS_STRICT_WINDING} + "'}}";
        BSONObj obj = fromjson(
            "{ 'type': 'GeometryCollection', 'geometries': ["
            "{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]] ]," +
            strictCRS +
            "}"
            "]}");
        mongo::GeometryCollection gc;
        ASSERT_OK(GeoParser::parseGeometryCollection(obj, false, &gc));
        ASSERT_EQ(gc.polygons[0]->crs, STRICT_SPHERE);
    }

    {
        string strictCRS =
            "crs:{ type: 'name', properties:{name:'" + std::string{CRS_STRICT_WINDING} + "'}}";
        BSONObj obj = fromjson(
            "{ 'type': 'GeometryCollection', 'geometries': ["
            "{ 'type': 'Point','coordinates': [100.0,0.0]},"
            "{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]] ]," +
            strictCRS +
            "}"
            "]}");
        mongo::GeometryCollection gc;
        ASSERT_OK(GeoParser::parseGeometryCollection(obj, false, &gc));
        ASSERT_EQ(gc.polygons[0]->crs, STRICT_SPHERE);
    }
}

// Note: in the following testcase comments, we use B, B', B'' to represent vertices that
// have 1-ULP-adjacent longitude values. They may collapse into identical S2Points.


// Ring: [A, B, B', A]
// A polygon ring where two adjacent vertices have distinct GeoJSON coordinates but
// map to the same S2Point under some sin/cos implementations (e.g. glibc 2.34) must be
// accepted consistently across platforms.
TEST(GeoParser, adjacentVertexCollapseFromSinCosRoundingIsRecoveredWhenFeatureFlagEnabled) {
    unittest::ServerParameterGuard featureFlag{"featureFlagGeoSinCosRoundingRecovery", true};
    PolygonWithCRS polygon;
    ASSERT_OK(GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon','coordinates':[[[0,0],[6.500000000000086,0],"
                 "[6.500000000000087,0],[0,0]]]}"),
        false,
        &polygon));
    // Triangle [A, B, B']: recovery separates B and B' when they collapse, and when
    // they don't collapse the ring is already a triangle.
    assertSingleLoopVertexCount(polygon, 3);
}

// Ring: [A, B, B', A]. With recovery disabled, whether this is rejected depends on whether B
// and B' actually collapse on the current glibc: if they do, the ring is reduced to 2 unique
// vertices and rejected; if they don't, it's already a valid triangle.
TEST(GeoParser, adjacentVertexCollapseIsRejectedWhenFeatureFlagDisabled) {
    unittest::ServerParameterGuard featureFlag{"featureFlagGeoSinCosRoundingRecovery", false};
    PolygonWithCRS polygon;
    bool collapses = collapsesOnCurrentGlibc(0, 6.500000000000086, 6.500000000000087);
    Status status = GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon','coordinates':[[[0,0],[6.500000000000086,0],"
                 "[6.500000000000087,0],[0,0]]]}"),
        false,
        &polygon);
    if (!collapses) {
        ASSERT_OK(status);
        assertSingleLoopVertexCount(polygon, 3);
        return;
    }
    ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "Loop must have at least 3 different vertices");
}

// Ring: [A, B, B, A].  Truly duplicated adjacent vertices. Two B will be reduced to one.
// The ring must still be rejected, regardless of the feature flag.
TEST(GeoParser, trulyDuplicateAdjacentVerticesAreRejectedRegardlessOfFeatureFlag) {
    for (bool flagEnabled : {true, false}) {
        unittest::ServerParameterGuard featureFlag{"featureFlagGeoSinCosRoundingRecovery",
                                                   flagEnabled};
        PolygonWithCRS polygon;
        Status status = GeoParser::parseGeoJSONPolygon(
            fromjson("{'type':'Polygon','coordinates':[[[0,0],[6.5,0],[6.5,0],[0,0]]]}"),
            false,
            &polygon);
        ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "Loop must have at least 3 different vertices");
        ASSERT_STRING_CONTAINS(status.reason(), "2 unique vertices were provided");
        // No vertex collapsed and no recovery happened.
        ASSERT_STRING_OMITS(status.reason(), "after recovery attempted");
    }
}

// Ring [A, B, B', B'', A]: Three distinct, 1-ULP-adjacent longitude values that ALL collapse
// to the same S2Point on glibc 2.34+.
TEST(GeoParser, tripleAdjacentVertexCollapseFromSinCosRoundingIsRecoveredWhenFeatureFlagEnabled) {
    unittest::ServerParameterGuard featureFlag{"featureFlagGeoSinCosRoundingRecovery", true};
    PolygonWithCRS polygon;
    ASSERT_OK(GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon','coordinates':[[[0,0],[-31.021928199999991,0],"
                 "[-31.021928199999987,0],[-31.021928199999984,0],[0,0]]]}"),
        false,
        &polygon));

    const bool firstPairCollapses =
        collapsesOnCurrentGlibc(0, -31.021928199999991, -31.021928199999987);
    const bool secondPairCollapses =
        collapsesOnCurrentGlibc(0, -31.021928199999987, -31.021928199999984);
    // Quadrilateral [A, B, B', B''] when both pairs collapse (recovery separates all three B's) and
    // when neither collapses (the ring is already a quadrilateral). If exactly one pair collapses,
    // the ring still has 3 vertices after deduplication, so recovery never runs and that collapsed
    // pair stays merged.
    assertSingleLoopVertexCount(polygon, firstPairCollapses == secondPairCollapses ? 4 : 3);
}

// Ring [A, B, B', B'', A]. With recovery disabled, this is only reduced below 3 unique
// vertices (and thus rejected) if BOTH adjacent pairs [B, B'] and [B', B''] collapse on the current
// glibc, cascading the whole run down to a single point; if only one pair collapses (or
// neither), the ring still ends up with >= 3 distinct vertices and is valid.
TEST(GeoParser, tripleAdjacentVertexCollapseIsRejectedWhenFeatureFlagDisabled) {
    unittest::ServerParameterGuard featureFlag{"featureFlagGeoSinCosRoundingRecovery", false};
    PolygonWithCRS polygon;
    bool bothCollapse = collapsesOnCurrentGlibc(0, -31.021928199999991, -31.021928199999987) &&
        collapsesOnCurrentGlibc(0, -31.021928199999987, -31.021928199999984);
    Status status = GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon','coordinates':[[[0,0],[-31.021928199999991,0],"
                 "[-31.021928199999987,0],[-31.021928199999984,0],[0,0]]]}"),
        false,
        &polygon);
    if (!bothCollapse) {
        ASSERT_OK(status);
        return;
    }
    ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "Loop must have at least 3 different vertices");
    ASSERT_STRING_CONTAINS(status.reason(), "2 unique vertices were provided");
}

// Ring [A, A', A', A'', Z, A].
// Original S2Points: [PA, PA, PA, PA, PZ, PA]
// After resize: [PA, PA, PA, PA, PZ]
// After recover: [PA, PA+1, PA+1, PA+2, PZ]
// After eraseDuplicatePoints: [PA, PA+1, PA+2, PZ]
TEST(GeoParser, duplicateCollapseFromSinCosRoundingIsRecoveredWhenFeatureFlagEnabled) {
    unittest::ServerParameterGuard featureFlag{"featureFlagGeoSinCosRoundingRecovery", true};
    PolygonWithCRS polygon;
    ASSERT_OK(GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon','coordinates':[[[-31.021928199999991,0],"
                 "[-31.021928199999987,0],[-31.021928199999987,0],[-31.021928199999984,0],"
                 "[0,0],[-31.021928199999991,0]]]}"),
        false,
        &polygon));

    const bool firstPairCollapses =
        collapsesOnCurrentGlibc(0, -31.021928199999991, -31.021928199999987);
    const bool secondPairCollapses =
        collapsesOnCurrentGlibc(0, -31.021928199999987, -31.021928199999984);
    // [PA, PA+1, PA+2, PZ] when both pairs collapse, per the walkthrough above; [PA, PA', PA'', PZ]
    // when neither collapses. If exactly one pair collapses, deduplication already leaves 3
    // vertices, so recovery never runs and that collapsed pair stays merged.
    assertSingleLoopVertexCount(polygon, firstPairCollapses == secondPairCollapses ? 4 : 3);
}

// Ring [A, A', A', A'', Z, A]. With recovery disabled, this is only reduced below 3 unique
// vertices (and thus rejected) if BOTH adjacent pairs (A-A', A'-A'') collapse down to a single
// point; otherwise the ring still ends up with >= 3 distinct vertices and is valid.
TEST(GeoParser, duplicateCollapseIsRejectedWhenFeatureFlagDisabled) {
    unittest::ServerParameterGuard featureFlag{"featureFlagGeoSinCosRoundingRecovery", false};
    PolygonWithCRS polygon;
    bool bothCollapse = collapsesOnCurrentGlibc(0, -31.021928199999991, -31.021928199999987) &&
        collapsesOnCurrentGlibc(0, -31.021928199999987, -31.021928199999984);
    Status status = GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon','coordinates':[[[-31.021928199999991,0],"
                 "[-31.021928199999987,0],[-31.021928199999987,0],[-31.021928199999984,0],"
                 "[0,0],[-31.021928199999991,0]]]}"),
        false,
        &polygon);
    if (!bothCollapse) {
        ASSERT_OK(status);
        return;
    }
    ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "Loop must have at least 3 different vertices");
    ASSERT_STRING_CONTAINS(status.reason(), "2 unique vertices were provided");
}

// Ring [A, B, X, Y, B', C, D, B'', A]: recoverCollapsedVerticesByShift() correctly detects
// and shifts all three as members of the same collapse group regardless of adjacency.
//
// However, the recovered ring still fails end-to-end, because it has 2 edges that cross.

// TODO(SERVER-132874): once RobustCrossing's sin/cos-rounding sign-flip bug is fixed, this
// ring is expected to become valid (ASSERT_OK) rather than fail on "Edges ... cross".
TEST(GeoParser,
     tripleNonAdjacentVertexCollapseFromSinCosRoundingIsRecoveredWhenFeatureFlagEnabled) {
    unittest::ServerParameterGuard featureFlag{"featureFlagGeoSinCosRoundingRecovery", true};
    PolygonWithCRS polygon;

    Status status = GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon','coordinates':[[[0,0],[-31.021928199999991,0],"
                 "[0,1],[1,0],[-31.021928199999987,0],[2,2],[3,3],"
                 "[-31.021928199999984,0],[0,0]]]}"),
        false,
        &polygon);
    ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "Loop is not valid");

    // `recoverCollapsedVertices` is successful, but the recovered loop still fails
    // S2Loop::IsValid() with "Edges x and y cross".
    // Once SERVER-132874 is fixed, this ring is expected to become valid instead.
    ASSERT_STRING_CONTAINS(status.reason(), "Edges 0 and 3 cross");
    // Recovery was attempted, but the recovered ring is still invalid.
    ASSERT_STRING_CONTAINS(status.reason(), "after recovery attempted");
}

// Ring: [A, B, B', B, B', B, A] should fail, since there are multiple B or B', which
// breaks the pair-wise distinct vertices requirement enforced in S2Loop.isValid().
TEST(GeoParser,
     repeatedLiteralCoordinatesAcrossNonAdjacentPositionsAreRejectedWhenFeatureFlagEnabled) {
    unittest::ServerParameterGuard featureFlag{"featureFlagGeoSinCosRoundingRecovery", true};
    PolygonWithCRS polygon;
    Status status5 = GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon','coordinates':[[[0,0],[6.500000000000086,0],"
                 "[6.500000000000087,0],[6.500000000000086,0],[6.500000000000087,0],"
                 "[6.500000000000086,0],[0,0]]]}"),
        false,
        &polygon);
    ASSERT_EQUALS(ErrorCodes::BadValue, status5.code());
    ASSERT_STRING_CONTAINS(status5.reason(), "Loop is not valid");
    ASSERT_STRING_CONTAINS(status5.reason(), "Duplicate vertices: 1 and 3");
    // Recovery only runs when B and B' actually collapse on glibc 2.34+.
    if (collapsesOnCurrentGlibc(0, 6.500000000000086, 6.500000000000087)) {
        ASSERT_STRING_CONTAINS(status5.reason(), "after recovery attempted");
    }
}

// Ring: [A, Z, A', A]: A (opening) and A' collapse but are not adjacent, while A' and A
// (closing) collapse and adjacent.
TEST(GeoParser, nonAdjacentVertexCollapseFromSinCosRoundingIsRecoveredWhenFeatureFlagEnabled) {
    unittest::ServerParameterGuard featureFlag{"featureFlagGeoSinCosRoundingRecovery", true};
    PolygonWithCRS polygon;
    ASSERT_OK(GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon','coordinates':[[[6.500000000000086,0],[0,0],"
                 "[6.500000000000087,0],[6.500000000000086,0]]]}"),
        false,
        &polygon));
    // Triangle [A, Z, A'] either way: recovery separates A' from the closing A when they collapse,
    // and when they don't collapse the ring is already a triangle.
    assertSingleLoopVertexCount(polygon, 3);
}

// Ring: [A, Z, A', A]. With recovery disabled, this is only rejected if A and A' actually
// collapse  - in which case A'(idx=2) and A(idx=3) merge into a single
// point during the initial adjacent-duplicate pass.
TEST(GeoParser, nonAdjacentVertexCollapseIsRejectedWhenFeatureFlagDisabled) {
    unittest::ServerParameterGuard featureFlag{"featureFlagGeoSinCosRoundingRecovery", false};
    PolygonWithCRS polygon;
    bool collapses = collapsesOnCurrentGlibc(0, 6.500000000000086, 6.500000000000087);
    Status status = GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon','coordinates':[[[6.500000000000086,0],[0,0],"
                 "[6.500000000000087,0],[6.500000000000086,0]]]}"),
        false,
        &polygon);
    if (!collapses) {
        ASSERT_OK(status);
        return;
    }
    ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "Loop must have at least 3 different vertices");
    ASSERT_STRING_CONTAINS(status.reason(), "2 unique vertices were provided");
}

// Ring: [A, B, B', A, A] -- a doubled closing vertex (the literal coordinate A repeated at
// idx0, idx3, and idx4) combined with a non-adjacent collapse.
TEST(GeoParser, doubledClosingVertexWithAdjacentCollapseIsRecoveredWhenFeatureFlagEnabled) {
    unittest::ServerParameterGuard featureFlag{"featureFlagGeoSinCosRoundingRecovery", true};
    PolygonWithCRS polygon;
    ASSERT_OK(GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon','coordinates':[[[0,0],[6.500000000000086,0],"
                 "[6.500000000000087,0],[0,0],[0,0]]]}"),
        false,
        &polygon));
    // Triangle [A, B, B'] either way: the doubled closing A collapses back to a single vertex, and
    // B / B' are separated by recovery when they collapse.
    assertSingleLoopVertexCount(polygon, 3);
}

TEST(GeoParser, doubledClosingVertexWithAdjacentCollapseIsRejectedWhenFeatureFlagDisabled) {
    unittest::ServerParameterGuard featureFlag{"featureFlagGeoSinCosRoundingRecovery", false};
    PolygonWithCRS polygon;
    bool collapses = collapsesOnCurrentGlibc(0, 6.500000000000086, 6.500000000000087);
    Status status = GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon','coordinates':[[[0,0],[6.500000000000086,0],"
                 "[6.500000000000087,0],[0,0],[0,0]]]}"),
        false,
        &polygon);
    if (!collapses) {
        ASSERT_OK(status);
        return;
    }
    ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "Loop must have at least 3 different vertices");
    ASSERT_STRING_CONTAINS(status.reason(), "2 unique vertices were provided");
}

// An interior ring [A, B, B', A] forms a hole in a polygon.
TEST(GeoParser, interiorRingVertexCollapseFromSinCosRoundingIsRecoveredWhenFeatureFlagEnabled) {
    unittest::ServerParameterGuard featureFlag{"featureFlagGeoSinCosRoundingRecovery", true};
    PolygonWithCRS polygon;
    ASSERT_OK(GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon','coordinates':["
                 "[[-1,-1],[10,-1],[10,2],[-1,2],[-1,-1]],"
                 "[[0,0],[6.500000000000086,0],[6.500000000000087,0],[0,0]]"
                 "]}"),
        false,
        &polygon));
    // Recovery separates B and B' when they collapse. If they do not collapse, the hole is
    // already a triangle.
    ASSERT(polygon.s2Polygon);
    ASSERT_EQUALS(2, polygon.s2Polygon->num_loops());
    ASSERT_EQUALS(4, polygon.s2Polygon->loop(0)->num_vertices());
    ASSERT_EQUALS(3, polygon.s2Polygon->loop(1)->num_vertices());
    ASSERT_TRUE(polygon.s2Polygon->loop(1)->is_hole());
}

// Same polygon with recovery disabled: rejected only if the hole's two vertices actually collapse
// on the current glibc, in which case the hole is left with 2 unique vertices.
TEST(GeoParser, interiorRingVertexCollapseIsRejectedWhenFeatureFlagDisabled) {
    unittest::ServerParameterGuard featureFlag{"featureFlagGeoSinCosRoundingRecovery", false};
    PolygonWithCRS polygon;
    bool collapses = collapsesOnCurrentGlibc(0, 6.500000000000086, 6.500000000000087);
    Status status = GeoParser::parseGeoJSONPolygon(
        fromjson("{'type':'Polygon','coordinates':["
                 "[[-1,-1],[10,-1],[10,2],[-1,2],[-1,-1]],"
                 "[[0,0],[6.500000000000086,0],[6.500000000000087,0],[0,0]]"
                 "]}"),
        false,
        &polygon);
    if (!collapses) {
        ASSERT_OK(status);
        ASSERT(polygon.s2Polygon);
        ASSERT_EQUALS(2, polygon.s2Polygon->num_loops());
        ASSERT_EQUALS(4, polygon.s2Polygon->loop(0)->num_vertices());
        ASSERT_EQUALS(3, polygon.s2Polygon->loop(1)->num_vertices());
        ASSERT_TRUE(polygon.s2Polygon->loop(1)->is_hole());
        return;
    }
    ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "Loop must have at least 3 different vertices");
    ASSERT_STRING_CONTAINS(status.reason(), "2 unique vertices were provided");
}

// MultiPolygon. The first member has a collapsing ring, the second is unaffected.
TEST(GeoParser, multiPolygonVertexCollapseFromSinCosRoundingIsRecoveredWhenFeatureFlagEnabled) {
    unittest::ServerParameterGuard featureFlag{"featureFlagGeoSinCosRoundingRecovery", true};
    mongo::MultiPolygonWithCRS mp;
    ASSERT_OK(GeoParser::parseMultiPolygon(
        fromjson("{'type':'MultiPolygon','coordinates':["
                 "[[[0,0],[6.500000000000086,0],[6.500000000000087,0],[0,0]]],"
                 "[[[100,0],[101,0],[101,1],[100,1],[100,0]]]"
                 "]}"),
        false,
        &mp));
    ASSERT_EQUALS((size_t)2, mp.polygons.size());
    // Triangle either way, exactly as for the standalone Polygon case.
    ASSERT_EQUALS(1, mp.polygons[0]->num_loops());
    ASSERT_EQUALS(3, mp.polygons[0]->loop(0)->num_vertices());
    // The unaffected member is untouched.
    ASSERT_EQUALS(1, mp.polygons[1]->num_loops());
    ASSERT_EQUALS(4, mp.polygons[1]->loop(0)->num_vertices());
}

// Same MultiPolygon with recovery disabled.
TEST(GeoParser, multiPolygonVertexCollapseIsRejectedWhenFeatureFlagDisabled) {
    unittest::ServerParameterGuard featureFlag{"featureFlagGeoSinCosRoundingRecovery", false};
    mongo::MultiPolygonWithCRS mp;
    bool collapses = collapsesOnCurrentGlibc(0, 6.500000000000086, 6.500000000000087);
    Status status = GeoParser::parseMultiPolygon(
        fromjson("{'type':'MultiPolygon','coordinates':["
                 "[[[0,0],[6.500000000000086,0],[6.500000000000087,0],[0,0]]],"
                 "[[[100,0],[101,0],[101,1],[100,1],[100,0]]]"
                 "]}"),
        false,
        &mp);
    if (!collapses) {
        ASSERT_OK(status);
        ASSERT_EQUALS((size_t)2, mp.polygons.size());
        // Triangle either way, exactly as for the standalone Polygon case.
        ASSERT_EQUALS(1, mp.polygons[0]->num_loops());
        ASSERT_EQUALS(3, mp.polygons[0]->loop(0)->num_vertices());
        // The unaffected member is untouched.
        ASSERT_EQUALS(1, mp.polygons[1]->num_loops());
        ASSERT_EQUALS(4, mp.polygons[1]->loop(0)->num_vertices());
        return;
    }
    ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "Loop must have at least 3 different vertices");
    ASSERT_STRING_CONTAINS(status.reason(), "2 unique vertices were provided");
}

// A GeometryCollection containing a strict-winding polygon parses successfully, but getNativeCRS()
// returns STRICT_SPHERE. The existing guards in S2GetKeysForElement (index path) and geoContains
// (non-index path) both check getNativeCRS() == STRICT_SPHERE and return early before any null
// s2Polygon is dereferenced.
TEST(GeoParser, strictPolygonInGeometryCollectionReportsCRS) {
    string strictCRS =
        "crs:{ type: 'name', properties:{name:'" + std::string{CRS_STRICT_WINDING} + "'}}";

    BSONObj storedDoc = fromjson(
        "{'type':'GeometryCollection','geometries':["
        "{'type':'Polygon','coordinates':[[[0,0],[5,0],[5,5],[0,5],[0,0]]]," +
        strictCRS +
        "}"
        "]}");
    GeometryContainer storedGeom;
    ASSERT_OK(storedGeom.parseFromStorage(BSON("geo" << storedDoc)["geo"]));
    ASSERT_EQ(storedGeom.getNativeCRS(), STRICT_SPHERE);
}

class GeoParserStrictWindingDeathTest : public unittest::Test {
protected:
    void setUp() override {
        std::string strictCRS =
            "crs:{ type: 'name', properties:{name:'" + std::string{CRS_STRICT_WINDING} + "'}}";
        BSONObj strictGCDoc = fromjson(
            "{'type':'GeometryCollection','geometries':["
            "{'type':'Polygon','coordinates':[[[0,0],[5,0],[5,5],[0,5],[0,0]]]," +
            strictCRS +
            "}"
            "]}");
        ASSERT_OK(strictGC.parseFromStorage(BSON("geo" << strictGCDoc)["geo"]));

        ASSERT_OK(point.parseFromStorage(
            BSON("geo" << fromjson("{'type':'Point','coordinates':[1,1]}"))["geo"]));
        ASSERT_OK(line.parseFromStorage(
            BSON("geo" << fromjson("{'type':'LineString','coordinates':[[1,1],[2,2]]}"))["geo"]));
        ASSERT_OK(polygon.parseFromStorage(
            BSON("geo" << fromjson("{'type':'Polygon','coordinates':[[[0,0],[2,0],[2,2],[0,2],"
                                   "[0,0]]]}"))["geo"]));
    }

    GeometryContainer strictGC;
    GeometryContainer point;
    GeometryContainer line;
    GeometryContainer polygon;
};

class GeoParserStrictWindingTest : public GeoParserStrictWindingDeathTest {};

// A GeometryCollection containing a strict-winding polygon parses successfully; getNativeCRS()
// resolves to STRICT_SPHERE, and hasS2Region() correctly reports false, so a correctly-behaving
// caller that gates on either never reaches the tasserts exercised below.
TEST_F(GeoParserStrictWindingTest, ReportsStrictSphereWithNoS2Region) {
    ASSERT_EQ(strictGC.getNativeCRS(), STRICT_SPHERE);
    ASSERT_FALSE(strictGC.hasS2Region());
}

// getS2Region() is separately guarded: reaching it despite hasS2Region() being false fails loudly
// rather than dereferencing a null _s2Region.
DEATH_TEST_F(GeoParserStrictWindingDeathTest, GetS2RegionTasserts, "12748600") {
    strictGC.getS2Region();
}

// Reaching the GeometryCollection member loops with a null s2Polygon fails loudly rather than
// crashing, regardless of which contains()/intersects() overload is dispatched to. All 7 call
// sites share getGeometryCollectionPolygonRegion()'s single crs-check tassert.
DEATH_TEST_F(GeoParserStrictWindingDeathTest, ContainsPointTasserts, "12748601") {
    strictGC.contains(point);
}

DEATH_TEST_F(GeoParserStrictWindingDeathTest, ContainsLineTasserts, "12748601") {
    strictGC.contains(line);
}

DEATH_TEST_F(GeoParserStrictWindingDeathTest, ContainsPolygonTasserts, "12748601") {
    strictGC.contains(polygon);
}

DEATH_TEST_F(GeoParserStrictWindingDeathTest, IntersectsPointTasserts, "12748601") {
    strictGC.intersects(point);
}

DEATH_TEST_F(GeoParserStrictWindingDeathTest, IntersectsLineTasserts, "12748601") {
    strictGC.intersects(line);
}

DEATH_TEST_F(GeoParserStrictWindingDeathTest, IntersectsPolygonTasserts, "12748601") {
    strictGC.intersects(polygon);
}

// intersects(const GeometryContainer&) also asserts while iterating a GeometryCollection passed in
// as the *other* argument, independent of what 'this' is.
DEATH_TEST_F(GeoParserStrictWindingDeathTest,
             IntersectsOtherGeometryCollectionTasserts,
             "12748601") {
    point.intersects(strictGC);
}
TEST(GeoParser, shiftRecoveryHandlesAllGlibcDivergentPairs) {
    unittest::ServerParameterGuard featureFlag{"featureFlagGeoSinCosRoundingRecovery", true};
    // Pairs where glibc 2.26 and glibc 2.34+ produce different S2Points for
    // 1-ULP-adjacent coordinates.
    // The following pairs are picked out of 1 million random samples with 1 ULP difference in
    // longitude or latitude coordinate and verified to produce different S2Points with one
    // platform, but produce same S2Points with another platform.

    // The shift recovery in geoparser.cpp must accept all of them regardless of which glibc version
    // is in use.
    struct Pair {
        double lng1, lat1, lng2, lat2;
    };
    // clang-format off
    static const Pair kPairs[] = {
        {130.01115870745917, 9.5150961652256463, 130.01115870745917, 9.5150961652256481},
        {-31.021928243402691, -70.023390281409661, -31.021928243402687, -70.023390281409661},
        {-103.18953812580604, 9.3307869830284353, -103.18953812580604, 9.3307869830284371},
        {17.684478202608894, 25.826312824630186, 17.684478202608894, 25.82631282463019},
        {-20.865604664436439, 33.918011270168421, -20.865604664436436, 33.918011270168421},
        {59.517849588985982, -22.542120376346844, 59.517849588985982, -22.542120376346841},
        {-37.758299513385154, 78.368307332871325, -37.758299513385147, 78.368307332871325},
        {-37.458735327205019, 12.347753526309621, -37.458735327205019, 12.347753526309623},
        {20.455916374600072, 58.487754606508226, 20.455916374600076, 58.487754606508226},
        {-65.768686750737004, -10.951677589408821, -65.768686750737004, -10.951677589408819},
        {-41.436701691442366, 17.004426692656434, -41.436701691442359, 17.004426692656434},
        {26.705448210871346, -7.2816139406209093, 26.705448210871346, -7.2816139406209084},
        {-0.34554982020019054, -14.496850650767158, -0.34554982020019048, -14.496850650767158},
        {-169.67944125581323, 14.158010804624322, -169.67944125581323, 14.158010804624324},
        {-9.7158607491543272, -45.513373795518945, -9.7158607491543254, -45.513373795518945},
        {22.086014351280888, 78.337743327198723, 22.086014351280891, 78.337743327198723},
        {19.617037583477106, -89.836768083236578, 19.61703758347711, -89.836768083236578},
        {-12.326595021485897, 68.574099244738264, -12.326595021485895, 68.574099244738264},
        {-20.307527516175561, -12.267773460037365, -20.307527516175561, -12.267773460037363},
        {179.91142505679278, 52.550033891453722, 179.91142505679278, 52.550033891453729},
        {41.959996410775148, 84.351609569210297, 41.959996410775155, 84.351609569210297},
        {-16.221796863959231, -6.6534022113461884, -16.221796863959227, -6.6534022113461884},
        {166.61355019670592, -24.351076883529768, 166.61355019670592, -24.351076883529764},
        {-65.648178982800999, -4.2228774757976435, -65.648178982800999, -4.2228774757976426},
        {-105.30898356750514, -11.074153062605635, -105.30898356750514, -11.074153062605633},
        {-61.036739886603904, 48.140430660093472, -61.036739886603904, 48.140430660093479},
        {75.956426839390119, -17.188444377534562, 75.956426839390119, -17.188444377534559},
        {147.60591524096003, 50.409084784134635, 147.60591524096003, 50.409084784134642},
        {-15.368052668280956, -47.508431263822708, -15.368052668280955, -47.508431263822708},
        {7.2544462888974381, -20.510860953604713, 7.2544462888974381, -20.510860953604709},
        {12.134659756214855, -16.048081957882399, 12.134659756214857, -16.048081957882399},
        {-53.719814705981562, -10.471508542463472, -53.719814705981562, -10.47150854246347},
        {174.16919371662712, 10.525080133013351, 174.16919371662712, 10.525080133013352},
        {-15.333077415347123, 24.152171396068567, -15.333077415347121, 24.152171396068567},
        {86.738845436016135, -27.535231802209019, 86.738845436016135, -27.535231802209015},
        {129.9403874655786, -10.507959126595313, 129.9403874655786, -10.507959126595312},
        {10.08058109970872, -6.2850448349285966, 10.080581099708722, -6.2850448349285966},
        {-169.57446437277537, -26.017639765158957, -169.57446437277537, -26.017639765158954},
        {6.1737259264563393, -23.502513093317361, 6.1737259264563393, -23.502513093317358},
        {22.25755501455496, 89.849219996954304, 22.257555014554963, 89.849219996954304},
        {19.466328119807351, 79.696962765381897, 19.466328119807354, 79.696962765381897},
        {17.136732349063102, -27.43195773076912, 17.136732349063106, -27.43195773076912},
        {-21.216653084904216, 20.879606954101789, -21.216653084904213, 20.879606954101789},
        {-78.772182807444651, 33.410525785460422, -78.772182807444651, 33.410525785460429},
        {-146.28068861305866, 28.41184805304912, -146.28068861305866, 28.411848053049123},
        {8.8291834592787346, -1.8831436960502146, 8.8291834592787364, -1.8831436960502146},
        {-67.578046297261977, 22.994838904847626, -67.578046297261977, 22.994838904847629},
        {-95.60024391914466, -16.240996264833168, -95.60024391914466, -16.240996264833164},
        {47.143179818479354, -26.642416293566864, 47.143179818479354, -26.64241629356686},
        {-30.07133347617841, 17.615038384523512, -30.07133347617841, 17.615038384523515},
        {26.683142008844108, -77.587441690034879, 26.683142008844111, -77.587441690034879},
        {-53.864219638136873, -12.27462115084727, -53.864219638136873, -12.274621150847269},
        {73.62430222797876, 14.21640134746357, 73.62430222797876, 14.216401347463572},
        {174.01435973337198, 9.0715642264250143, 174.01435973337198, 9.071564226425016},
        {-0.87733402461715571, -7.0643493912179824, -0.87733402461715571, -7.0643493912179816},
        {-12.807479070105753, -24.988848635905949, -12.807479070105751, -24.988848635905949},
        {168.05960202792303, 21.743853056250913, 168.05960202792303, 21.743853056250916},
        {47.675520254622597, -47.437596743010594, 47.675520254622604, -47.437596743010594},
        {10.136008712785948, -35.928551029422898, 10.13600871278595, -35.928551029422898},
        {-111.54129825482319, -55.003991946201594, -111.54129825482319, -55.003991946201587},
        {10.374921090564836, -74.256519013388825, 10.374921090564838, -74.256519013388825},
        {-31.610948404751063, 18.337965141349766, -31.610948404751063, 18.33796514134977},
        {-27.108283363122638, 23.795211404679691, -27.108283363122634, 23.795211404679691},
        {97.038460076054633, 8.3780411567401654, 97.038460076054633, 8.3780411567401671},
        {-23.809041249276255, 2.1675614616575345, -23.809041249276252, 2.1675614616575345},
        {44.97671321009642, -17.178204175178731, 44.97671321009642, -17.178204175178728},
        {-111.5374168906357, 43.672121799249823, -111.5374168906357, 43.67212179924983},
        {13.290144529996265, -72.801501260935538, 13.290144529996267, -72.801501260935538},
        {133.92519188334799, -44.22098730404285, 133.92519188334799, -44.220987304042843},
        {-22.737593679812143, -34.570311318731171, -22.737593679812139, -34.570311318731171},
        {10.510293940641414, -66.744061150885074, 10.510293940641416, -66.744061150885074},
        {36.726464545303138, -18.116984021392167, 36.726464545303138, -18.116984021392163},
        {77.601813002654509, 16.385353205449416, 77.601813002654509, 16.38535320544942},
        {-172.61396717617029, -34.263409066501836, -172.61396717617029, -34.263409066501829},
        {-38.903924309050026, 8.6363709535498181, -38.903924309050026, 8.6363709535498199},
        {-75.796347340660802, 23.143156364483175, -75.796347340660802, 23.143156364483179},
        {-134.79732692630716, -31.694125376231387, -134.79732692630716, -31.694125376231383},
        {12.888235893983445, -86.333876629359366, 12.888235893983447, -86.333876629359366},
        {-151.4793933862089, 17.736954923611091, -151.4793933862089, 17.736954923611094},
        {-26.244297901671018, 66.563120140225379, -26.244297901671015, 66.563120140225379},
        {67.161937248635041, 19.345177032688763, 67.161937248635041, 19.345177032688767},
        {53.326536177132041, -13.26301716736198, 53.326536177132049, -13.26301716736198},
        {104.33828896279493, 17.197053767795747, 104.33828896279493, 17.197053767795751},
        {-148.76629617029795, 38.19628538419213, -148.76629617029795, 38.196285384192137},
        {151.84844800650896, -21.637979902058476, 151.84844800650896, -21.637979902058472},
        {20.332068355412794, 6.0016567311925524, 20.332068355412797, 6.0016567311925524},
        {-16.807766512809639, -6.5296329927652703, -16.807766512809636, -6.5296329927652703},
        {-125.0004094727995, 19.150965897877537, -125.0004094727995, 19.150965897877541},
        {-103.32692682209257, -16.593368674504656, -103.32692682209257, -16.593368674504653},
        {77.978860703782189, -20.701995730534541, 77.978860703782189, -20.701995730534538},
        {-26.209001308218831, -34.641688600819847, -26.209001308218827, -34.641688600819847},
        {-14.825995866614353, -16.77551261376524, -14.825995866614353, -16.775512613765237},
        {-20.868250329053105, -13.66048943954938, -20.868250329053101, -13.66048943954938},
        {149.80075898011296, 25.646683629940263, 149.80075898011296, 25.646683629940267},
        {12.191141129115802, -78.496808681559529, 12.191141129115804, -78.496808681559529},
        {26.286190021884728, 50.02955522692055, 26.286190021884732, 50.02955522692055},
        {-83.236127744344174, 9.7875987290554214, -83.236127744344174, 9.7875987290554232},
        {10.70495837909942, 23.204363369084476, 10.704958379099422, 23.204363369084476},
        {2.9404468915820559, -22.411655122542669, 2.9404468915820563, -22.411655122542669},
        {-32.570986781953813, 16.004526103090083, -32.570986781953806, 16.004526103090083},
        {173.25108647418202, 11.122674856446725, 173.25108647418202, 11.122674856446727},
        {18.60810437904221, -20.274329800726285, 18.60810437904221, -20.274329800726282},
        {26.964115143423022, -16.430543160218345, 26.964115143423022, -16.430543160218342},
        {-14.07678277393514, -59.122276070531719, -14.076782773935138, -59.122276070531719},
        {8.6011663872441737, 16.41442670842795, 8.6011663872441755, 16.41442670842795},
        {20.445288244286964, 81.777270881511214, 20.445288244286967, 81.777270881511214},
        {-145.49683157429445, 9.1455753707850143, -145.49683157429445, 9.145575370785016},
        {105.72730180492684, -7.3264719045735607, 105.72730180492684, -7.3264719045735598},
        {-99.130670701459707, -9.0212979912693161, -99.130670701459707, -9.0212979912693143},
        {-52.759228130471563, -26.274172924496195, -52.759228130471563, -26.274172924496192},
        {-24.926349019971788, -15.410188000160844, -24.926349019971788, -15.410188000160842},
        {-61.000491385161844, 24.307200746766256, -61.000491385161844, 24.307200746766259},
        {160.01129366514226, -55.716645781739182, 160.01129366514226, -55.716645781739174},
        {-11.042338682737491, 65.597265626965381, -11.042338682737489, 65.597265626965381},
        {167.31791720644833, -20.367740503978062, 167.31791720644833, -20.367740503978059},
        {171.52612294817285, -12.844456596593275, 171.52612294817285, -12.844456596593274},
        {134.60525673761649, -11.93966937469327, 134.60525673761649, -11.939669374693269},
        {151.96640889987711, -12.346561425928332, 151.96640889987711, -12.34656142592833},
        {129.1438807476427, 8.7798584236806967, 129.1438807476427, 8.7798584236806985},
        {46.349515029352887, 20.746271947443702, 46.349515029352894, 20.746271947443702},
        {34.53459181194679, 8.3051354409205498, 34.53459181194679, 8.3051354409205516},
        {-126.96630113022006, 9.9676758402210943, -126.96630113022006, 9.9676758402210961},
        {-84.321405121593017, 13.217409187487309, -84.321405121593017, 13.217409187487311},
        {78.405636933362715, -8.4047122933872433, 78.405636933362715, -8.4047122933872416},
        {44.056148168863785, -84.309841509492841, 44.056148168863793, -84.309841509492841},
        {27.614437861461177, 65.18229341275449, 27.61443786146118, 65.18229341275449},
        {139.35497022284164, 12.84385098987649, 139.35497022284164, 12.843850989876492},
        {-55.535246868189162, 10.654643863661878, -55.535246868189162, 10.65464386366188},
        {-170.32630883230507, -17.184476276392171, -170.32630883230507, -17.184476276392168},
        {-60.338487206691134, -7.7433727741158922, -60.338487206691134, -7.7433727741158913},
        {-44.190221051158943, -11.103167658428536, -44.190221051158936, -11.103167658428536},
        {7.718633712499039, -44.108056891662656, 7.7186337124990398, -44.108056891662656},
        {163.72460477575569, -12.786967225781268, 163.72460477575569, -12.786967225781266},
        {19.726540063194456, 11.398196427221738, 19.726540063194456, 11.398196427221739},
        {118.52538952935711, 14.115960527580238, 118.52538952935711, 14.11596052758024},
        {-67.437014750210722, 34.770144245610425, -67.437014750210722, 34.770144245610432},
        {17.530849388279684, -70.875986494145309, 17.530849388279687, -70.875986494145309},
        {113.00954742076901, 43.225645837155241, 113.00954742076901, 43.225645837155248},
        {18.740375807704062, 88.642252043190055, 18.740375807704066, 88.642252043190055},
        {-46.770963244524864, -17.908996152479258, -46.770963244524857, -17.908996152479258},
        {-163.39833661134301, -17.780100905843835, -163.39833661134301, -17.780100905843831},
        {-25.805428185883159, -11.516513943474042, -25.805428185883159, -11.51651394347404},
        {92.798145718464127, -11.453953774979485, 92.798145718464127, -11.453953774979484},
        {44.591560424131217, -10.473387845649501, 44.591560424131217, -10.4733878456495},
        {-78.001679951782492, 26.95959794547263, -78.001679951782492, 26.959597945472634},
        {18.265289667702696, 16.285136093362052, 18.265289667702699, 16.285136093362052},
        {14.999172395896178, -67.490250793185268, 14.99917239589618, -67.490250793185268},
        {24.765891514581007, 56.852481598190913, 24.765891514581011, 56.852481598190913},
        {-13.056743150857212, -11.94621386380199, -13.056743150857212, -11.946213863801988},
        {177.40578101841749, 46.317617060471918, 177.40578101841749, 46.317617060471925},
        {33.317395530060786, 80.517610597981559, 33.317395530060793, 80.517610597981559},
        {-40.95919008017141, -5.7076484661026523, -40.95919008017141, -5.7076484661026514},
        {74.458116459764696, -8.2683870901165992, 74.458116459764696, -8.2683870901165974},
        {-10.489503095469214, -14.045237587259521, -10.489503095469212, -14.045237587259521},
        {122.89512222101175, -25.675609337332457, 122.89512222101175, -25.675609337332453},
        {177.42910994174309, -19.730132538862755, 177.42910994174309, -19.730132538862751},
        {14.104175043711077, 10.91552875528436, 14.104175043711079, 10.91552875528436},
        {-46.485428867592532, -9.4639800352920336, -46.485428867592532, -9.4639800352920318},
        {50.967641596334502, -17.516199401789503, 50.967641596334509, -17.516199401789503},
        {50.311390345714329, 28.815003194891695, 50.311390345714337, 28.815003194891695},
        {-24.732169909174083, -18.769626179461731, -24.732169909174079, -18.769626179461731},
        {97.934591639588575, -12.275231616513768, 97.934591639588575, -12.275231616513766},
        {-90.161454626945073, 43.244444202903111, -90.161454626945073, 43.244444202903118},
        {124.61656307976349, -13.099849522837928, 124.61656307976349, -13.099849522837927},
        {-46.163366315133288, -67.799651296309719, -46.163366315133281, -67.799651296309719},
        {-3.3616170390949152, 2.8240859699079679, -3.3616170390949152, 2.8240859699079683},
        {22.631860321016724, 9.9713051808283417, 22.631860321016724, 9.9713051808283435},
        {-18.127323377630223, -50.656102058463262, -18.127323377630219, -50.656102058463262},
        {-10.158756502502481, -80.319736444364949, -10.158756502502479, -80.319736444364949},
        {-88.407280667246766, -11.028611964520042, -88.407280667246766, -11.02861196452004},
        {46.226244752115889, 9.0028081798408301, 46.226244752115889, 9.0028081798408319},
        {-6.4965843640551153, 11.681068981917523, -6.4965843640551144, 11.681068981917523},
        {-8.8201381026575767, -12.214176093096544, -8.8201381026575767, -12.214176093096542},
        {-14.285262879152885, 19.003146638569699, -14.285262879152883, 19.003146638569699},
        {27.475257670730009, -13.268242077006121, 27.475257670730009, -13.26824207700612},
        {-104.99218295778662, 11.416173091110412, -104.99218295778662, 11.416173091110414},
        {54.325674139229378, -18.013253822409492, 54.325674139229378, -18.013253822409489},
        {-126.75052281699446, -20.491985007151463, -126.75052281699446, -20.49198500715146},
        {-19.181954927344538, -3.5548456751137278, -19.181954927344535, -3.5548456751137278},
        {42.851150248819692, -12.273638133030364, 42.851150248819692, -12.273638133030362},
        {9.9758276526823089, -70.560529878789637, 9.9758276526823106, -70.560529878789637},
        {-42.150364307126083, -17.685984878185817, -42.150364307126083, -17.685984878185813},
        {124.44719355890535, -46.619011114577077, 124.44719355890535, -46.61901111457707},
        {8.5526150630057565, 31.238888434171642, 8.5526150630057582, 31.238888434171642},
        {-6.6947434196489493, 83.646277086731246, -6.6947434196489484, 83.646277086731246},
        {-163.31803524400181, 19.415533511158525, -163.31803524400181, 19.415533511158529},
        {-121.31600287240438, -6.1089293022924647, -121.31600287240438, -6.1089293022924638},
        {-58.917037056474854, -28.005461213020048, -58.917037056474854, -28.005461213020045},
        {-46.375671723754351, -36.008335841814251, -46.375671723754344, -36.008335841814251},
        {-81.350378410155102, -8.1477600200431457, -81.350378410155102, -8.1477600200431439},
        {-166.6032208798263, -12.21545276569778, -166.6032208798263, -12.215452765697778},
        {134.69260822771784, -10.497458296831827, 134.69260822771784, -10.497458296831825},
        {63.981554092834784, -11.796513779608237, 63.981554092834784, -11.796513779608235},
        {-18.826035546308749, -6.0485011054927735, -18.826035546308749, -6.0485011054927726},
        {-11.430617663702204, 38.196200373912994, -11.430617663702202, 38.196200373912994},
        {-134.42731819257244, 9.1343399162931185, -134.42731819257244, 9.1343399162931203},
        {-20.883185630717954, 57.303295150631918, -20.88318563071795, 57.303295150631918},
        {15.1572934946476, 9.1263873567925948, 15.1572934946476, 9.1263873567925966},
        {-102.7183047378165, 12.731810564619654, -102.7183047378165, 12.731810564619655},
        {47.317911733515473, -46.799715663431755, 47.31791173351548, -46.799715663431755},
        {127.4063246046172, 36.440924395011095, 127.4063246046172, 36.440924395011102},
        {-17.633518235539032, -54.936484857187097, -17.633518235539029, -54.936484857187097},
        {31.784430127988138, -20.468780754193325, 31.784430127988138, -20.468780754193322},
        {-64.205992786480834, 34.289398121683035, -64.205992786480834, 34.289398121683043},
        {0.8706969419142041, -32.52377255664905, 0.8706969419142041, -32.523772556649043},
        {15.811168836093369, -18.422973005048426, 15.811168836093371, -18.422973005048426},
        {-113.67713519823472, 51.232250178843799, -113.67713519823472, 51.232250178843806},
        {-64.602009714134709, -10.17022447899571, -64.602009714134709, -10.170224478995708},
        {53.460480111720287, 19.097865563378161, 53.460480111720294, 19.097865563378161},
        {17.494371635998512, -24.637483707635806, 17.494371635998515, -24.637483707635806},
        {178.77361766356529, -10.342760881237542, 178.77361766356529, -10.34276088123754},
        {-131.39958919821819, -12.778437866504163, -131.39958919821819, -12.778437866504161},
        {5.8216202274230655, -1.9301406296717716, 5.8216202274230664, -1.9301406296717716},
        {47.455771709972737, 21.734670356783539, 47.455771709972737, 21.734670356783543},
        {36.317637411662723, -14.745888914323274, 36.31763741166273, -14.745888914323274},
        {103.26880092344851, 40.42452948331853, 103.26880092344851, 40.424529483318537},
        {112.60374515651269, 28.047510274681777, 112.60374515651269, 28.04751027468178},
        {-96.998678266996151, -27.942058918108891, -96.998678266996151, -27.942058918108888},
        {104.51329997112737, 12.298576734186998, 104.51329997112737, 12.298576734187},
        {-4.0599309028554327, 10.576088659831417, -4.0599309028554327, 10.576088659831418},
        {-160.33127053384399, 17.312276728345164, -160.33127053384399, 17.312276728345168},
        {-9.6589488785691593, 66.646591013680151, -9.6589488785691575, 66.646591013680151},
        {-125.90724597355045, -15.495018252458717, -125.90724597355045, -15.495018252458715},
        {89.914568841411977, 10.939903277844042, 89.914568841411977, 10.939903277844044},
        {-35.632392452544842, 36.385088533508721, -35.632392452544835, 36.385088533508721},
        {-20.356707845497517, 36.381811917316909, -20.356707845497517, 36.381811917316917},
        {-162.01085158953256, 23.612879955456989, -162.01085158953256, 23.612879955456993},
        {9.2260185754076609, -51.630752827158666, 9.2260185754076627, -51.630752827158666},
        {178.43832447507864, 23.032462875557769, 178.43832447507864, 23.032462875557773},
        {37.117222907319039, -78.471795303040992, 37.117222907319046, -78.471795303040992},
        {-3.2965962278778194, 10.627182791093336, -3.2965962278778194, 10.627182791093338},
        {46.786730486829612, -26.24976432338725, 46.786730486829619, -26.24976432338725},
        {5.2114273695864455, 25.584876203287685, 5.2114273695864455, 25.584876203287688},
        {-161.80188826932104, -53.515719357389756, -161.80188826932104, -53.515719357389749},
        {-135.62795307316105, -9.6435621351483238, -135.62795307316105, -9.6435621351483221},
        {102.80367655558777, 13.584785859825676, 102.80367655558777, 13.584785859825677},
        {42.191071787351802, 10.145798552271902, 42.191071787351802, 10.145798552271904},
        {61.831784972221854, -26.291564171680957, 61.831784972221854, -26.291564171680953},
        {22.561249324720627, -44.237734496845945, 22.561249324720627, -44.237734496845938},
        {12.58764084423856, -0.8123327245917622, 12.587640844238562, -0.8123327245917622},
        {-17.98658941095211, 1.792789841442719, -17.986589410952107, 1.792789841442719},
        {22.597001782372963, 73.218546862347239, 22.597001782372967, 73.218546862347239},
        {93.117816964381149, -16.435778874760583, 93.117816964381149, -16.43577887476058},
        {73.677270546935958, -10.46878726636557, 73.677270546935958, -10.468787266365569},
        {56.214806670485572, -9.8072376492913751, 56.214806670485579, -9.8072376492913751},
        {1.2061593539493032, 10.554038494870287, 1.2061593539493032, 10.554038494870289},
        {93.10393197043166, 13.194661735490389, 93.10393197043166, 13.19466173549039},
        {-170.230425591791, 53.083227253938361, -170.230425591791, 53.083227253938368},
        {-171.9446474189341, -19.102551322073786, -171.9446474189341, -19.102551322073783},
        {-14.012396686346559, -44.182479698499563, -14.012396686346557, -44.182479698499563},
        {-172.49286493458126, -19.890158881763504, -172.49286493458126, -19.890158881763501},
        {44.369842839560164, -7.1261827522273027, 44.369842839560171, -7.1261827522273027},
        {67.413975710401985, -16.708025919163301, 67.413975710401985, -16.708025919163298},
        {162.27713734318974, -55.243757153823069, 162.27713734318974, -55.243757153823061},
        {-148.06625172207066, 7.9644330501375844, -148.06625172207066, 7.9644330501375853},
        {155.13070733370958, 15.833493918981983, 155.13070733370958, 15.833493918981985},
        {9.1561732337923409, 76.639627938626333, 9.1561732337923427, 76.639627938626333},
        {-70.089957961817248, -43.703297058243024, -70.089957961817248, -43.703297058243017},
        {-8.7026723352508064, 25.170381997067082, -8.7026723352508046, 25.170381997067082},
        {-176.31006371264377, -6.1724972995578593, -176.31006371264377, -6.1724972995578584},
        {-13.939233107668198, 22.235845187123189, -13.939233107668196, 22.235845187123189},
        {0.50139525078442659, 24.014030036113681, 0.50139525078442659, 24.014030036113684},
        {54.736076790504868, 13.499436284241359, 54.736076790504868, 13.49943628424136},
        {11.47801049572902, -67.819398643696459, 11.478010495729022, -67.819398643696459},
        {146.70450317893514, 54.436903711708972, 146.70450317893514, 54.436903711708979},
        {19.255115403547201, 28.417206792040226, 19.255115403547201, 28.417206792040229},
        {68.57208126597402, 26.713864382821761, 68.57208126597402, 26.713864382821765},
        {152.53662768137792, -24.037737447971974, 152.53662768137792, -24.037737447971971},
        {-33.044553087137182, 52.993353564915608, -33.044553087137174, 52.993353564915608},
        {-101.36936860863315, -55.143488561439838, -101.36936860863315, -55.143488561439831},
        {-154.09291400895106, -16.00737557086094, -154.09291400895106, -16.007375570860937},
        {-62.892374812533099, 16.813950650353771, -62.892374812533099, 16.813950650353775},
        {-8.1096259321291537, 18.138187783025071, -8.1096259321291537, 18.138187783025074},
        {150.33817369401066, -24.414577897353649, 150.33817369401066, -24.414577897353645},
        {-21.128448382353049, 32.343099777078535, -21.128448382353046, 32.343099777078535},
        {140.53983733749445, -28.422165464852107, 140.53983733749445, -28.422165464852103},
        {26.680618380153767, 26.785598466706865, 26.680618380153771, 26.785598466706865},
        {-24.354954230772712, 58.0701160733039, -24.354954230772709, 58.0701160733039},
        {6.720165458465428, -19.225504408734899, 6.7201654584654289, -19.225504408734899},
        {-31.94610366825875, -51.87364368574827, -31.94610366825875, -51.873643685748263},
        {-91.088882057852459, 19.336818335553357, -91.088882057852459, 19.33681833555336},
        {47.145110520559484, -22.266791532104165, 47.145110520559484, -22.266791532104161},
        {-10.016076472769665, -26.495066247702805, -10.016076472769663, -26.495066247702805},
        {89.758736307956866, 8.1939934014384459, 89.758736307956866, 8.1939934014384477},
        {-157.25337415146035, 8.2497001350708228, -157.25337415146035, 8.2497001350708246},
        {-22.949851256587841, -65.524901879993806, -22.949851256587838, -65.524901879993806},
        {-6.0838871039157478, -84.084940996279215, -6.0838871039157469, -84.084940996279215},
        {18.865598975247487, -71.495203853801641, 18.86559897524749, -71.495203853801641},
        {12.320530746144808, -40.253035442795557, 12.32053074614481, -40.253035442795557},
        {55.238117719318737, 28.84482824996055, 55.238117719318744, 28.84482824996055},
        {-124.25100221745546, -54.360174324289844, -124.25100221745546, -54.360174324289837},
        {-156.64723288171407, 9.9682272693135623, -156.64723288171407, 9.9682272693135641},
        {-5.1796014187235979, -8.6591620021885465, -5.1796014187235979, -8.6591620021885447},
        {55.468485177809001, -17.172080828646823, 55.468485177809001, -17.17208082864682},
        {63.233597633726532, 19.489694506892715, 63.233597633726532, 19.489694506892718},
        {-40.792447158579037, -47.682921073490753, -40.792447158579037, -47.682921073490746},
        {155.98324260446225, -13.691769850330019, 155.98324260446225, -13.691769850330017},
        {10.822166790082658, 21.553004825288838, 10.82216679008266, 21.553004825288838},
        {51.268690514755789, 23.983744773621297, 51.268690514755797, 23.983744773621297},
        {171.87588071536294, 21.010730951278774, 171.87588071536294, 21.010730951278777},
        {12.594464063307353, 79.049544034418872, 12.594464063307354, 79.049544034418872},
        {-171.98479472381817, 11.695252940622044, -171.98479472381817, 11.695252940622046},
        {-146.3183088515031, 44.683019863969434, -146.3183088515031, 44.683019863969442},
        {-65.767953779232073, -45.024023556158625, -65.767953779232073, -45.024023556158618},
        {10.103234016482471, -24.66643740536254, 10.103234016482473, -24.66643740536254},
        {-16.404745601437003, -83.989205521060825, -16.404745601437, -83.989205521060825},
        {149.65610391517021, -15.368253958157553, 149.65610391517021, -15.368253958157551},
        {178.0133001122816, -10.615077300175816, 178.0133001122816, -10.615077300175814},
        {-9.1258399688237226, -14.263721200662275, -9.1258399688237208, -14.263721200662275},
        {64.363691926659243, 10.17198524935019, 64.363691926659243, 10.171985249350191},
        {106.08327213374878, -17.573662496277674, 106.08327213374878, -17.57366249627767},
        {-175.45150687693945, 22.991128185406467, -175.45150687693945, 22.99112818540647},
        {-62.118072607464974, -15.879703012061295, -62.118072607464974, -15.879703012061293},
        {25.153343333806937, -65.790777992570909, 25.153343333806941, -65.790777992570909},
        {96.291974350820908, -8.2671004873735345, 96.291974350820908, -8.2671004873735328},
        {-50.488568955980888, -14.21895972962696, -50.488568955980888, -14.218959729626958},
        {137.92294874469076, -24.360120056547228, 137.92294874469076, -24.360120056547224},
        {7.9695792790043329, -42.469123508581717, 7.9695792790043338, -42.469123508581717},
        {-70.351848698747588, 17.071454792049845, -70.351848698747588, 17.071454792049849},
        {-31.031414985074598, 12.404066928019692, -31.031414985074598, 12.404066928019693},
        {170.98024005756682, 50.495536032887642, 170.98024005756682, 50.495536032887649},
        {30.403383096041146, -21.996085331022542, 30.403383096041146, -21.996085331022538},
        {-172.38635473404611, -11.441719663890172, -172.38635473404611, -11.44171966389017},
        {-112.88466401144139, -13.123610703686111, -112.88466401144139, -13.12361070368611},
        {-23.8089865693101, -12.761165887433506, -23.8089865693101, -12.761165887433505},
        {-19.827743439963264, 10.377779239997484, -19.82774343996326, 10.377779239997484},
        {-146.09474745208277, -4.4835008286883333, -146.09474745208277, -4.4835008286883324},
        {18.701487793920826, -60.044440035090254, 18.70148779392083, -60.044440035090254},
        {-22.95259853564361, -13.809779076265528, -22.952598535643606, -13.809779076265528},
        {-143.16292066819713, -10.572838313784704, -143.16292066819713, -10.572838313784702},
        {162.75067531858403, -7.8913970865054441, 162.75067531858403, -7.8913970865054432},
        {73.315585735411943, 51.898824380961059, 73.315585735411943, 51.898824380961067},
        {-27.23040532596886, 29.192757858867957, -27.230405325968857, 29.192757858867957},
        {44.219240130022342, 8.417172012506084, 44.219240130022342, 8.4171720125060858},
        {157.77603433899066, 21.670987536186502, 157.77603433899066, 21.670987536186505},
        {-8.73344110001565, 52.361964809198653, -8.7334411000156482, 52.361964809198653},
        {-46.010064697443141, 26.284162128986985, -46.010064697443134, 26.284162128986985},
        {80.50675265611595, -32.472540697942321, 80.50675265611595, -32.472540697942314},
        {-164.95144004860131, 56.774352988670408, -164.95144004860131, 56.774352988670415},
        {-136.4657513738554, 10.887235441836726, -136.4657513738554, 10.887235441836728},
        {36.531394990969567, 14.076579119596627, 36.531394990969567, 14.076579119596628},
        {71.134410668843799, -26.667833881817966, 71.134410668843799, -26.667833881817963},
        {-166.82938674653678, -10.186549828546635, -166.82938674653678, -10.186549828546633},
        {-49.954111213305303, -88.981825959646557, -49.954111213305296, -88.981825959646557},
        {-91.288302981470764, -27.135621228487061, -91.288302981470764, -27.135621228487057},
        {55.699295376087136, 84.030321230287853, 55.699295376087143, 84.030321230287853},
        {-135.86518408855881, -13.982838106984815, -135.86518408855881, -13.982838106984813},
        {-38.356283893004885, 86.339340672006188, -38.356283893004878, 86.339340672006188},
        {-70.848373332932695, 16.71436500759955, -70.848373332932695, 16.714365007599554},
        {-149.45563299628421, -20.641624155324187, -149.45563299628421, -20.641624155324184},
        {147.88012068037818, -30.116438856302143, 147.88012068037818, -30.116438856302139},
        {-12.584290633353529, -59.454540853830302, -12.584290633353527, -59.454540853830302},
        {19.052443941324533, 22.076760248884824, 19.052443941324537, 22.076760248884824},
        {-83.12027632236402, -13.486408586656978, -83.12027632236402, -13.486408586656976},
        {12.213420328242645, 22.822112501586076, 12.213420328242647, 22.822112501586076},
        {138.79616668882474, -15.131650257397183, 138.79616668882474, -15.131650257397181},
        {-12.235592406306747, 31.509881171984247, -12.235592406306745, 31.509881171984247},
        {-46.715425167760799, 8.2339494747370718, -46.715425167760799, 8.2339494747370736},
        {-78.821508249193002, 19.550432308054393, -78.821508249193002, 19.550432308054397},
        {120.84876349937042, 17.699392542624988, 120.84876349937042, 17.699392542624992},
        {-162.92923674944782, -47.591431158999661, -162.92923674944782, -47.591431158999654},
        {104.00665002604086, -10.035802638326675, 104.00665002604086, -10.035802638326674},
        {140.00151792437433, 28.309684540774491, 140.00151792437433, 28.309684540774494},
        {-2.9702639012485066, -41.383555777536827, -2.9702639012485061, -41.383555777536827},
        {126.12728375215804, 9.0944878976275625, 126.12728375215804, 9.0944878976275643},
        {-61.728536824805225, 43.242421634510663, -61.728536824805225, 43.24242163451067},
        {66.02744408117178, 20.787259097528519, 66.02744408117178, 20.787259097528523},
        {162.459511500384, -9.7177426318663809, 162.459511500384, -9.7177426318663791},
        {160.20268085832703, 10.428779758204815, 160.20268085832703, 10.428779758204817},
        {-174.5036805679548, 10.47798448975357, -174.5036805679548, 10.477984489753572},
        {-26.342522214146015, -83.909603725393808, -26.342522214146012, -83.909603725393808},
        {-76.421495109422395, -25.131823419757477, -76.421495109422395, -25.131823419757474},
        {-179.23423825416705, 13.135789063795261, -179.23423825416705, 13.135789063795263},
        {-3.3273571693096415, 9.5588623005814846, -3.3273571693096415, 9.5588623005814863},
        {-172.18112334365512, -51.742700301848579, -172.18112334365512, -51.742700301848572},
        {-21.059279681818175, 8.7916478349148299, -21.059279681818175, 8.7916478349148317},
        {37.908111940066838, -41.407925532609624, 37.908111940066838, -41.407925532609617},
        {166.06055510875106, 27.142343408236222, 166.06055510875106, 27.142343408236226},
        {-6.8281957547426426, 8.740899528487919, -6.8281957547426426, 8.7408995284879207},
        {-10.275621419009976, -23.839192122200046, -10.275621419009976, -23.839192122200043},
        {-156.01408853865442, 22.077736503696215, -156.01408853865442, 22.077736503696219},
        {16.863410687907642, 9.2222658182712571, 16.863410687907646, 9.2222658182712571},
        {-11.057030054447065, -63.107835932766513, -11.057030054447063, -63.107835932766513},
        {-137.14159164294301, 22.1514975732902, -137.14159164294301, 22.151497573290204},
        {-102.53654144617556, -11.337967718161014, -102.53654144617556, -11.337967718161012},
        {-14.039530968842339, -2.7257824687037866, -14.039530968842337, -2.7257824687037866},
        {159.46271349959267, -20.921224939413214, 159.46271349959267, -20.921224939413211},
        {17.175128639730993, -37.820839928287334, 17.175128639730996, -37.820839928287334},
        {-50.795301760497402, 78.313655771606989, -50.795301760497395, 78.313655771606989},
        {-11.094167695429999, 18.227886752181728, -11.094167695429999, 18.227886752181732},
        {8.25782647355393, -57.172580803346548, 8.2578264735539317, -57.172580803346548},
        {-7.9463411813361002, -12.458621374752571, -7.9463411813360993, -12.458621374752571},
        {163.93991981551051, 22.663883575512543, 163.93991981551051, 22.663883575512546},
        {5.712497602770239, 28.027315563111578, 5.712497602770239, 28.027315563111582},
        {86.925629025657088, -14.306052920384161, 86.925629025657088, -14.306052920384159},
        {62.442711485315982, 27.493273905931652, 62.442711485315982, 27.493273905931655},
        {-78.538000569995106, 52.270930606894268, -78.538000569995106, 52.270930606894275},
        {-167.95589126859792, -13.236606986790221, -167.95589126859792, -13.236606986790219},
        {161.3660872386881, -35.10361999593998, 161.3660872386881, -35.103619995939972},
        {-160.53515945117235, -13.548659911134934, -160.53515945117235, -13.548659911134932},
        {-22.571767615132082, -13.695929288135403, -22.571767615132082, -13.695929288135401},
        {-15.124746777243264, -10.984608380563262, -15.124746777243264, -10.984608380563261},
        {-146.77139583495236, 9.1998029282387286, -146.77139583495236, 9.1998029282387304},
        {108.5258636930961, -24.295382304382809, 108.5258636930961, -24.295382304382805},
        {-110.73545505642048, -37.854305919217474, -110.73545505642048, -37.854305919217467},
        {-85.042168678216626, 3.6998005917142351, -85.042168678216626, 3.6998005917142356},
        {56.991919023582213, 14.046638046208958, 56.991919023582213, 14.04663804620896},
        {-20.390028409318582, -52.349736413796471, -20.390028409318582, -52.349736413796464},
        {64.848036574067748, -12.234843794768867, 64.848036574067748, -12.234843794768866},
        {-110.69146294659734, 3.6618208460924806, -110.69146294659734, 3.661820846092481},
        {-10.989253542630419, -84.13870749589654, -10.989253542630417, -84.13870749589654},
        {-33.967430404614213, 54.915587760877095, -33.967430404614213, 54.915587760877102},
        {-29.409330424639847, 6.905192417402743, -29.409330424639847, 6.9051924174027439},
        {96.722431607445571, -6.90369967554558, 96.722431607445571, -6.9036996755455791},
        {45.04377388506407, -22.781693870642783, 45.043773885064077, -22.781693870642783},
        {-30.347140827827587, 10.589622234071573, -30.347140827827587, 10.589622234071575},
        {134.68944544905551, -12.846721522332075, 134.68944544905551, -12.846721522332073},
        {7.4238454878881699, -28.163413398575081, 7.4238454878881708, -28.163413398575081},
        {168.30208577035233, -11.37695590126221, 168.30208577035233, -11.376955901262209},
        {156.89802085068754, -8.7183801476235292, 156.89802085068754, -8.7183801476235274},
        {13.181817769699617, 20.864648063847937, 13.181817769699617, 20.864648063847941},
        {-24.934924321506891, -14.577369424101093, -24.934924321506891, -14.577369424101091},
        {144.69357372161102, 49.169403679840187, 144.69357372161102, 49.169403679840194},
        {-115.0140691161254, -27.6667768503983, -115.0140691161254, -27.666776850398296},
        {45.285727263586978, -23.590392291393314, 45.285727263586978, -23.590392291393311},
        {108.65287100588195, -5.112173720947963, 108.65287100588195, -5.1121737209479621},
        {-28.438039754272666, -4.5389062113841856, -28.438039754272666, -4.5389062113841847},
        {-5.844649815569289, -73.307993951283535, -5.8446498155692881, -73.307993951283535},
        {64.219098284081923, 31.061223657108684, 64.219098284081923, 31.061223657108687},
        {-171.80018432429787, 21.846072146898166, -171.80018432429787, 21.84607214689817},
        {-78.84925064019761, -10.674228242776142, -78.84925064019761, -10.67422824277614},
        {-91.714638922393277, -31.101406773425925, -91.714638922393277, -31.101406773425921},
        {166.55733496564889, 48.278911852887347, 166.55733496564889, 48.278911852887354},
        {97.138574468966112, 13.956431111846403, 97.138574468966112, 13.956431111846404},
        {1.2336868022419623, -5.4339478707243511, 1.2336868022419623, -5.4339478707243503},
        {-164.16631526888744, 43.679207990372987, -164.16631526888744, 43.679207990372994},
        {22.560141051882006, 39.59337986329583, 22.56014105188201, 39.59337986329583},
        {5.0389935405483044, 24.527890987866215, 5.0389935405483053, 24.527890987866215},
        {-5.0556068696031726, 13.75105648535467, -5.0556068696031726, 13.751056485354672},
        {-7.0712686742310833, 37.408892994480247, -7.0712686742310833, 37.408892994480254},
        {-25.056506452937704, -51.747662006475899, -25.056506452937704, -51.747662006475892},
        {-20.551798416146177, 11.07760503105624, -20.551798416146177, 11.077605031056242},
        {2.6562131829332003, 71.219946907562047, 2.6562131829332007, 71.219946907562047},
        {-115.6602501778132, -9.1932300997914442, -115.6602501778132, -9.1932300997914425},
        {7.3202401864966449, -39.06163216742609, 7.3202401864966458, -39.06163216742609},
        {60.369269821374289, 11.406460550677696, 60.369269821374289, 11.406460550677698},
        {-13.11538424468122, 50.328901115329785, -13.115384244681218, 50.328901115329785},
        {163.57104262803344, 50.473157865677791, 163.57104262803344, 50.473157865677798},
        {44.234525597338546, -41.276119033136041, 44.234525597338553, -41.276119033136041},
        {-22.675360310682755, -83.578550951777586, -22.675360310682752, -83.578550951777586},
        {16.444747640026062, -44.276348229244377, 16.444747640026065, -44.276348229244377},
        {144.29422252049437, -20.339319687935067, 144.29422252049437, -20.339319687935063},
        {-10.566217920919424, -45.388777103939745, -10.566217920919422, -45.388777103939745},
        {47.690843702424758, -5.135387509447737, 47.690843702424758, -5.1353875094477361},
        {-146.71265068464268, 20.459409357845757, -146.71265068464268, 20.459409357845761},
        {-111.60396336329573, 52.007681430152743, -111.60396336329573, 52.007681430152751},
        {99.288090825646975, -25.386978449944717, 99.288090825646975, -25.386978449944714},
        {-8.692422569604016, -45.756690803210475, -8.6924225696040143, -45.756690803210475},
        {26.278063702151123, 74.774100549620584, 26.278063702151126, 74.774100549620584},
        {28.020348306935723, -65.22843608423598, 28.020348306935727, -65.22843608423598},
        {-44.061950791833425, -59.89349144236666, -44.061950791833418, -59.89349144236666},
        {100.53511217985331, 40.463551719677859, 100.53511217985331, 40.463551719677866},
        {-63.570739878234413, -12.435035481636238, -63.570739878234413, -12.435035481636236},
        {-9.6431802137479075, 8.4218912000703323, -9.6431802137479075, 8.4218912000703341},
        {-20.725422922132715, -84.423730169271266, -20.725422922132712, -84.423730169271266},
        {52.027289016590132, 3.2876983083109068, 52.027289016590132, 3.2876983083109073},
        {55.780844313113661, -34.629743773045277, 55.780844313113661, -34.62974377304527},
        {33.177819050899068, -7.3804444379447833, 33.177819050899068, -7.3804444379447824},
        {-52.614388441466211, 85.120560701788989, -52.614388441466204, 85.120560701788989},
        {23.507595574766505, -21.12154254996679, 23.507595574766505, -21.121542549966787},
        {10.127870190864282, 84.712997611678261, 10.127870190864284, 84.712997611678261},
        {2.8797929549836709, 14.975023609947041, 2.8797929549836709, 14.975023609947042},
        {73.867058889995789, -11.036105397415671, 73.867058889995789, -11.036105397415669},
        {178.42607747272623, -9.0577687881337656, 178.42607747272623, -9.0577687881337638},
        {18.397963877068698, -56.86566731507407, 18.397963877068701, -56.86566731507407},
        {178.81951777539186, -46.831467357563938, 178.81951777539186, -46.83146735756393},
        {-178.64767336354461, 24.470445262866292, -178.64767336354461, 24.470445262866296},
        {13.279151576637783, -17.708323608370502, 13.279151576637785, -17.708323608370502},
        {9.0941530770380687, 63.573056230717995, 9.0941530770380705, 63.573056230717995},
        {-25.254728549006138, 47.825199674328331, -25.254728549006135, 47.825199674328331},
        {161.93431238945641, 17.293338937304863, 161.93431238945641, 17.293338937304867},
        {106.66193752151713, 28.322514833006657, 106.66193752151713, 28.322514833006661},
        {43.202555882526298, 52.854227609970863, 43.202555882526305, 52.854227609970863},
        {-166.97257762254304, 26.498757539232535, -166.97257762254304, 26.498757539232539},
        {-136.92579360811322, 13.28497048618431, -136.92579360811322, 13.284970486184312},
        {-36.752596155506225, -7.4829385978950516, -36.752596155506225, -7.4829385978950507},
        {-13.228348944951701, 58.722727906087492, -13.2283489449517, 58.722727906087492},
        {-66.704355844792815, 27.80716860010741, -66.704355844792815, 27.807168600107413},
        {-12.401015587061547, -77.080700492396616, -12.401015587061545, -77.080700492396616},
        {51.979631012540736, -51.878627229306893, 51.979631012540736, -51.878627229306886},
        {-38.711443213645893, -88.635846139966503, -38.711443213645886, -88.635846139966503},
        {26.265015857426455, 61.697151521590328, 26.265015857426459, 61.697151521590328},
        {141.04442878151349, 25.795689813668609, 141.04442878151349, 25.795689813668613},
        {-12.717405480952632, 86.510656154806952, -12.71740548095263, 86.510656154806952},
        {-47.404592913419719, 9.0557552254929909, -47.404592913419719, 9.0557552254929927},
        {16.425548567055269, 78.102157666975984, 16.425548567055273, 78.102157666975984},
        {44.9404622634014, -53.667323274679163, 44.940462263401407, -53.667323274679163},
        {-55.051254341252317, 29.34827338006918, -55.05125434125231, 29.34827338006918},
        {30.996729506999593, 22.621567393386826, 30.996729506999596, 22.621567393386826},
        {-25.940082233173484, -35.110829724374085, -25.940082233173484, -35.110829724374078},
        {7.7863918466092885, 57.086314556776067, 7.7863918466092885, 57.086314556776074},
        {156.09466919382433, -27.044353764088875, 156.09466919382433, -27.044353764088871},
        {-116.52406418480331, 19.359648842176494, -116.52406418480331, 19.359648842176497},
        {-118.57110064367482, 4.6400865840647594, -118.57110064367482, 4.6400865840647603},
        {-53.016276138833653, -60.31636502303467, -53.016276138833646, -60.31636502303467},
        {-22.171082418760058, -70.979002880672439, -22.171082418760054, -70.979002880672439},
        {-8.0748508890254751, -72.300640283663583, -8.0748508890254733, -72.300640283663583},
        {-157.29114888313708, 8.6795575135841609, -157.29114888313708, 8.6795575135841627},
        {-20.329202679691754, -23.514187180841873, -20.32920267969175, -23.514187180841873},
        {-0.77174471992542237, -13.399273398447221, -0.77174471992542237, -13.399273398447219},
        {144.67573431335256, 17.314731615329418, 144.67573431335256, 17.314731615329421},
        {-9.192592577976594, -56.883923346588098, -9.1925925779765922, -56.883923346588098},
        {19.330538593864482, 15.407807701666865, 19.330538593864482, 15.407807701666867},
        {-109.91608140333088, 50.015534338163334, -109.91608140333088, 50.015534338163341},
        {46.283899785685009, 6.833760728048814, 46.283899785685016, 6.833760728048814},
        {175.32048202793888, -24.415936422493072, 175.32048202793888, -24.415936422493068},
        {172.21467148851883, -19.597743139234147, 172.21467148851883, -19.597743139234144},
        {114.46529973916445, -25.735235856956304, 114.46529973916445, -25.735235856956301},
        {123.95880136150504, 34.314557228383094, 123.95880136150504, 34.314557228383102},
        {-109.88450683165172, -7.6370831829093024, -109.88450683165172, -7.6370831829093015},
        {-9.6167202762185546, 42.905357453269332, -9.6167202762185529, 42.905357453269332},
        {24.80889471444798, -73.766617741807053, 24.808894714447984, -73.766617741807053},
        {-74.404783954738647, 7.6880165491264147, -74.404783954738647, 7.6880165491264156},
        {-4.299132473084148, 14.022640913576584, -4.299132473084148, 14.022640913576586},
        {94.330279355864917, -2.7362715690396042, 94.330279355864917, -2.7362715690396038},
        {138.44295383591697, -13.466959927810379, 138.44295383591697, -13.466959927810377},
        {-117.75002502078557, 23.038231439094375, -117.75002502078557, 23.038231439094378},
        {-104.39067902373468, 19.987752784582511, -104.39067902373468, 19.987752784582515},
        {-85.558795734660379, -9.6732275529448177, -85.558795734660379, -9.673227552944816},
        {63.234261247176164, -17.276686561505297, 63.234261247176164, -17.276686561505294},
        {151.84507175351786, 12.165146346568822, 151.84507175351786, 12.165146346568823},
        {-47.836242404598686, 9.6267632097698943, -47.836242404598686, 9.6267632097698961},
        {10.148557058600542, -80.498457037701996, 10.148557058600543, -80.498457037701996},
        {44.207652689009876, 11.839397367895049, 44.207652689009883, 11.839397367895049},
        {48.83380370428705, -8.8936274958239583, 48.83380370428705, -8.8936274958239565},
        {65.771224866413036, 50.31224753519183, 65.771224866413036, 50.312247535191837},
        {-56.336932238671821, -51.68578466147445, -56.336932238671821, -51.685784661474443},
        {23.658236387166632, -25.808089038971413, 23.658236387166632, -25.80808903897141},
        {99.036052209067549, -10.346463428397929, 99.036052209067549, -10.346463428397927},
        {-83.656638658859862, -3.3199505720832176, -83.656638658859862, -3.3199505720832172},
        {-92.438693969851016, -5.7897870357611776, -92.438693969851016, -5.7897870357611767},
        {-114.54025442251157, 52.61853762440488, -114.54025442251157, 52.618537624404887},
        {36.431924290288485, 40.105032835440575, 36.431924290288492, 40.105032835440575},
        {-58.546803895994785, -25.640271714869041, -58.546803895994785, -25.640271714869037},
        {13.594187598235234, 21.137557602401714, 13.594187598235234, 21.137557602401717},
        {-138.23105608777539, 10.349976057823017, -138.23105608777539, 10.349976057823019},
        {-115.57186382248537, -9.2222289523989733, -115.57186382248537, -9.2222289523989716},
        {147.82160313468526, -12.291380515513243, 147.82160313468526, -12.291380515513241},
        {138.25220525542005, -7.4907705134885507, 138.25220525542005, -7.4907705134885498},
        {135.97583414918387, -9.6219408682895082, 135.97583414918387, -9.6219408682895065},
        {-13.077851232634398, 26.084270103544888, -13.077851232634398, 26.084270103544892},
        {135.62073441910127, -13.30420054326575, 135.62073441910127, -13.304200543265749},
        {20.893312611486159, -53.979066115968962, 20.893312611486163, -53.979066115968962},
        {-44.378129277683996, -89.951432612812852, -44.378129277683989, -89.951432612812852},
        {-25.301459831468385, 70.312189053656041, -25.301459831468382, 70.312189053656041},
        {-23.179351182641177, 72.329542549167996, -23.179351182641174, 72.329542549167996},
        {65.705916766750107, 10.137382374649565, 65.705916766750107, 10.137382374649567},
        {-11.356471477305771, -82.712187328874521, -11.356471477305769, -82.712187328874521},
        {-4.8632167420134405, 86.553274585798604, -4.8632167420134396, 86.553274585798604},
        {-9.4566359576301942, 25.835493770734026, -9.4566359576301942, 25.835493770734029},
        {-37.462328400095757, 32.776111730910898, -37.46232840009575, 32.776111730910898},
        {121.60583311906004, 6.6680188570179251, 121.60583311906004, 6.668018857017926},
        {172.60967411649486, -38.713176316582256, 172.60967411649486, -38.713176316582249},
        {89.921015677399907, -8.713207697586606, 89.921015677399907, -8.7132076975866042},
        {-12.704592689337741, -87.210343879130704, -12.704592689337739, -87.210343879130704},
        {11.68330468624799, 9.618249334011896, 11.68330468624799, 9.6182493340118977},
        {113.561019191331, 22.660549940139035, 113.561019191331, 22.660549940139038},
        {-7.469307906190414, -66.618022436188639, -7.4693079061904131, -66.618022436188639},
        {12.340097511426759, -74.292214900128087, 12.340097511426761, -74.292214900128087},
        {-16.731706277515588, 74.498417420155306, -16.731706277515585, 74.498417420155306},
        {-127.2056157709029, -8.6224577099449053, -127.2056157709029, -8.6224577099449036},
        {-10.426036004871801, 53.283306022345869, -10.426036004871799, 53.283306022345869},
        {-96.736852413258404, 25.39139347425759, -96.736852413258404, 25.391393474257594},
        {-97.699987342925851, -5.7003929990748343, -97.699987342925851, -5.7003929990748334},
        {14.207505260584238, 20.305422875719088, 14.20750526058424, 20.305422875719088},
        {105.90337514008098, -30.993639676121067, 105.90337514008098, -30.993639676121063},
        {58.551712161503374, 43.223351193838283, 58.551712161503374, 43.22335119383829},
        {12.204173790377663, -69.096503157642744, 12.204173790377665, -69.096503157642744},
        {71.52897848942078, 54.33926272906961, 71.52897848942078, 54.339262729069617},
        {22.543065839026191, 28.535739909604693, 22.543065839026195, 28.535739909604693},
        {-75.907622902348677, -27.167559465045549, -75.907622902348677, -27.167559465045546},
        {71.635546126573956, -44.172007363891751, 71.635546126573956, -44.172007363891744},
        {7.9831922860651305, 8.6331315453636428, 7.9831922860651314, 8.6331315453636428},
        {-13.505869124420936, 11.582672908914805, -13.505869124420935, 11.582672908914805},
        {11.343457231774291, -62.725851050099713, 11.343457231774293, -62.725851050099713},
        {129.73206830141078, -31.748923465189382, 129.73206830141078, -31.748923465189378},
        {-121.81662972418358, 11.281596528700678, -121.81662972418358, 11.28159652870068},
        {97.407748207352114, 9.1568431381819018, 97.407748207352114, 9.1568431381819035},
        {46.175819953233571, 27.930019752480952, 46.175819953233571, 27.930019752480955},
        {-52.895843604318422, 56.15601677448938, -52.895843604318415, 56.15601677448938},
        {-66.745431244926181, 26.947148275827352, -66.745431244926181, 26.947148275827356},
        {-103.29812411385747, 11.980961687800619, -103.29812411385747, 11.980961687800621},
        {150.87475348425943, 22.70372021514757, 150.87475348425943, 22.703720215147573},
        {-98.887323393585973, 32.519388152849032, -98.887323393585973, 32.519388152849039},
        {-80.437332082813313, -12.873174127844194, -80.437332082813313, -12.873174127844193},
        {115.9435997466645, 13.018597015138557, 115.9435997466645, 13.018597015138559},
        {84.172777799164109, -21.178341293147554, 84.172777799164109, -21.17834129314755},
        {-51.421965342577032, -50.760372282410842, -51.421965342577032, -50.760372282410835},
        {11.108632856130608, -50.383107256068058, 11.108632856130608, -50.383107256068051},
        {111.9089592115093, -39.657253055726365, 111.9089592115093, -39.657253055726358},
        {9.9853794553437716, 81.572407480088799, 9.9853794553437734, 81.572407480088799},
        {-84.605822268417882, -25.131818568741249, -84.605822268417882, -25.131818568741245},
        {-12.632010028860602, -11.573487410856632, -12.6320100288606, -11.573487410856632},
        {-99.073747981960452, -3.1015581263350098, -99.073747981960452, -3.1015581263350094},
        {-23.435394911210501, -15.902355346047468, -23.435394911210498, -15.902355346047468},
        {-9.3627351160267516, -9.7795827754943279, -9.3627351160267516, -9.7795827754943261},
        {115.59557231930104, 25.320006315700631, 115.59557231930104, 25.320006315700635},
        {6.2813308302443405, -15.123738968434367, 6.2813308302443405, -15.123738968434365},
        {-142.91997264319704, -7.3546895871513076, -142.91997264319704, -7.3546895871513067},
        {-123.74891351249568, 25.225996335287178, -123.74891351249568, 25.225996335287181},
        {-57.407401598185132, -40.464891434363928, -57.407401598185132, -40.464891434363921},
        {87.442838228646849, 20.241757265368719, 87.442838228646849, 20.241757265368722},
        {-116.0815282937161, -47.846040753272867, -116.0815282937161, -47.84604075327286},
        {119.8457530794729, 10.482874963096339, 119.8457530794729, 10.482874963096341},
        {62.708541761002323, -16.362318359691866, 62.708541761002323, -16.362318359691862},
        {38.099071139080301, 27.531621118202516, 38.099071139080301, 27.531621118202519},
        {-21.332533219050454, -37.657864179313613, -21.33253321905045, -37.657864179313613},
        {15.436610534223941, -7.7716960511413848, 15.436610534223941, -7.771696051141384},
        {31.442758089918549, -11.030242872970252, 31.442758089918549, -11.03024287297025},
        {-25.05417027360452, -79.554166734754432, -25.054170273604516, -79.554166734754432},
        {-91.279682979799588, -25.984039585882623, -91.279682979799588, -25.98403958588262},
        {10.255333355619424, -44.895896275200172, 10.255333355619426, -44.895896275200172},
        {90.763027309025091, -9.6901330747055372, 90.763027309025091, -9.6901330747055354},
        {3.1231512573408082, -10.114845342699471, 3.1231512573408082, -10.11484534269947},
        {10.191408112008244, 2.4464134294262507, 10.191408112008245, 2.4464134294262507},
        {86.166444279767205, 14.960095689930125, 86.166444279767205, 14.960095689930126},
        {-14.621606440569924, -11.503542946707132, -14.621606440569922, -11.503542946707132},
        {4.9135600817619007, -61.41498625462053, 4.9135600817619016, -61.41498625462053},
        {69.310950974702934, -12.371765474282629, 69.310950974702934, -12.371765474282627},
        {-6.5990076765705847, -34.446807895204699, -6.5990076765705838, -34.446807895204699},
        {5.2783008110639429, -15.965908808334202, 5.2783008110639438, -15.965908808334202},
        {69.3554512172351, -14.125968116903863, 69.3554512172351, -14.125968116903861},
        {-24.288381784257929, -32.421117462534866, -24.288381784257925, -32.421117462534866},
        {-21.81032813459553, -73.050828956775234, -21.810328134595526, -73.050828956775234},
        {114.44093748144799, -9.5949109236747834, 114.44093748144799, -9.5949109236747816},
        {-139.62010039396745, 26.60979168818357, -139.62010039396745, 26.609791688183574},
        {-28.668688812333471, -32.091665601689314, -28.668688812333471, -32.091665601689307},
        {128.95988940449848, 23.420757628199567, 128.95988940449848, 23.420757628199571},
        {84.393116527283468, 26.343152019813793, 84.393116527283468, 26.343152019813797},
        {45.666081725759483, 14.631486886407739, 45.666081725759483, 14.631486886407741},
        {10.626071382242625, 13.829307552358049, 10.626071382242626, 13.829307552358049},
        {36.098138346853169, -20.946154955308067, 36.098138346853176, -20.946154955308067},
        {-72.386391070437284, 6.6218407509454931, -72.386391070437284, 6.621840750945494},
        {-18.080693651069584, -79.570740953777587, -18.080693651069581, -79.570740953777587},
        {-16.736422769520221, 74.987051171352746, -16.736422769520217, 74.987051171352746},
        {99.352630889275133, 19.767558216173285, 99.352630889275133, 19.767558216173288},
        {-162.00056371666344, -10.800832355312483, -162.00056371666344, -10.800832355312481},
        {-25.692624294576262, 18.121060656226671, -25.692624294576262, 18.121060656226675},
        {20.96284343732038, 41.256042277718201, 20.962843437320384, 41.256042277718201},
        {-37.388236766139968, 78.029849633695989, -37.388236766139961, 78.029849633695989},
        {102.58383569934466, -19.406853626185843, 102.58383569934466, -19.406853626185839},
        {26.80008228068062, 27.989374743474819, 26.80008228068062, 27.989374743474823},
        {-12.374858433200412, -51.09150989668187, -12.37485843320041, -51.09150989668187},
        {51.58711202375342, -10.131412785862754, 51.58711202375342, -10.131412785862752},
        {-60.481741318010059, -46.930532090404554, -60.481741318010059, -46.930532090404547},
        {132.61079602571638, -11.775743937389652, 132.61079602571638, -11.775743937389651},
        {12.462282105330441, -22.086377185517463, 12.462282105330441, -22.08637718551746},
        {-33.812203623888358, -37.381059423120838, -33.812203623888358, -37.381059423120831},
        {-13.203575721300037, 15.537688554375844, -13.203575721300036, 15.537688554375844},
        {63.524464479407669, -17.165568434092318, 63.524464479407669, -17.165568434092314},
        {44.750864269970748, -9.5617587384179306, 44.750864269970748, -9.5617587384179288},
        {-32.523259867437325, -88.365918977082032, -32.523259867437318, -88.365918977082032},
        {53.081949098697535, 2.6724805724956369, 53.081949098697535, 2.6724805724956373},
        {-7.5065865724934095, 81.800980183105864, -7.5065865724934087, 81.800980183105864},
        {-139.42977928222021, -18.590548256556509, -139.42977928222021, -18.590548256556506},
        {-95.938525752210481, 18.008987220813946, -95.938525752210481, 18.00898722081395},
        {-1.7091404293810086, 25.795273159897285, -1.7091404293810086, 25.795273159897288},
        {63.209447615169438, -3.4282697439552576, 63.209447615169438, -3.4282697439552572},
        {-45.058678284437633, -20.529033609481466, -45.058678284437633, -20.529033609481463},
        {-84.755842532030613, 38.23584270383666, -84.755842532030613, 38.235842703836667},
        {-13.146628040427654, 8.333537667453454, -13.146628040427654, 8.3335376674534558},
        {8.7698763758092149, -56.206802842561622, 8.7698763758092149, -56.206802842561615},
        {51.624412514269252, 14.092976674918514, 51.624412514269252, 14.092976674918516},
        {-14.067110309024844, 28.663539585603665, -14.067110309024843, 28.663539585603665},
        {-23.225870400496269, -15.798229784076366, -23.225870400496266, -15.798229784076366},
        {-18.523248089226588, 25.161977566251792, -18.523248089226584, 25.161977566251792},
        {92.318546559999504, -20.290512573980841, 92.318546559999504, -20.290512573980838},
        {-12.881451327221608, 67.04921383403024, -12.881451327221606, 67.04921383403024},
        {42.22256146689412, 24.514866044392363, 42.22256146689412, 24.514866044392367},
        {-144.67672277255889, -19.39714955542545, -144.67672277255889, -19.397149555425447},
        {-169.64739890521, -14.026309655974472, -169.64739890521, -14.026309655974471},
        {-17.173473249445966, -79.584632518575901, -17.173473249445962, -79.584632518575901},
        {166.5361453505457, -52.062061059705378, 166.5361453505457, -52.062061059705371},
        {176.35404832750098, 30.958517672204039, 176.35404832750098, 30.958517672204042},
        {145.5836521501904, -11.758888947789149, 145.5836521501904, -11.758888947789147},
        {91.821069668039684, 18.53863284299673, 91.821069668039684, 18.538632842996734},
        {8.6854334392070278, -20.848531770677532, 8.6854334392070296, -20.848531770677532},
        {-5.5238095374353176, -56.204791892785643, -5.5238095374353167, -56.204791892785643},
        {89.780163025816051, 25.406485767857411, 89.780163025816051, 25.406485767857415},
        {2.5350766728636387, -83.720279588118814, 2.5350766728636391, -83.720279588118814},
        {-174.1711346270547, -22.755226784921632, -174.1711346270547, -22.755226784921629},
        {44.812679941703465, -8.481401813257742, 44.812679941703472, -8.481401813257742},
        {-153.40711466780681, -12.727991427609183, -153.40711466780681, -12.727991427609181},
        {-18.02660359842405, 86.091686773651105, -18.026603598424046, 86.091686773651105},
        {86.589232392164035, -11.379290021024321, 86.589232392164035, -11.379290021024319},
        {7.2758443812898266, 34.329371312783877, 7.2758443812898275, 34.329371312783877},
        {-43.674409868551521, -31.027254244940398, -43.674409868551521, -31.027254244940394},
        {-85.005521461952128, 16.184907796747463, -85.005521461952128, 16.184907796747467},
        {94.806663754274581, 8.6849417234294926, 94.806663754274581, 8.6849417234294943},
        {18.671956736400837, -50.274559706801583, 18.671956736400837, -50.274559706801575},
        {30.81167170697929, -39.173470101711736, 30.811671706979293, -39.173470101711736},
        {-134.3041247979765, 5.12056964629876, -134.3041247979765, 5.1205696462987609},
        {9.732968092748532, -22.530089014293011, 9.732968092748532, -22.530089014293008},
        {-7.7921205347366591, 46.122170153749011, -7.7921205347366582, 46.122170153749011},
        {-27.455409815658555, -8.0150487102743941, -27.455409815658552, -8.0150487102743941},
        {12.814467326710822, 75.054674581349531, 12.814467326710824, 75.054674581349531},
        {10.576396471262481, -45.402542129759723, 10.576396471262482, -45.402542129759723},
        {158.55083394227611, 20.204196975348736, 158.55083394227611, 20.20419697534874},
        {102.64762556434192, 14.040184392403678, 102.64762556434192, 14.040184392403679},
        {25.520277779839809, 5.4415638256886307, 25.520277779839809, 5.4415638256886316},
        {23.958281915401621, 10.408034215962022, 23.958281915401624, 10.408034215962022},
        {-63.294617371324627, 21.691436956773458, -63.294617371324627, 21.691436956773462},
        {146.54296470983158, 26.66494280692681, 146.54296470983158, 26.664942806926813},
        {129.30004404161502, -9.547930149052485, 129.30004404161502, -9.5479301490524833},
        {8.92198924794425, -47.388705008185418, 8.9219892479442517, -47.388705008185418},
        {-24.470713339361374, -23.110461520946657, -24.470713339361371, -23.110461520946657},
        {63.505683606055811, 30.588416848316193, 63.505683606055811, 30.588416848316196},
        {46.808105329618797, -13.439926536082979, 46.808105329618805, -13.439926536082979},
        {160.36727185184495, 8.4179535405163914, 160.36727185184495, 8.4179535405163932},
        {-16.689431310023497, -50.780236012232791, -16.689431310023494, -50.780236012232791},
        {5.7036465369034461, -60.726590455875034, 5.703646536903447, -60.726590455875034},
        {94.15223419884083, -26.62688999833497, 94.15223419884083, -26.626889998334967},
        {-49.94666628099818, -88.06475726336636, -49.946666280998173, -88.06475726336636},
        {-169.75045322623683, -11.943704416549753, -169.75045322623683, -11.943704416549751},
        {-105.49665914514509, -54.653654174996134, -105.49665914514509, -54.653654174996127},
        {55.146557111813323, 79.201419138982516, 55.14655711181333, 79.201419138982516},
        {13.618447411264722, -63.152798757289951, 13.618447411264723, -63.152798757289951},
        {-127.82444143805871, 16.714430734681567, -127.82444143805871, 16.714430734681571},
        {-18.997243917719675, -12.643968767222095, -18.997243917719675, -12.643968767222093},
        {21.363654104294632, 28.042259052525786, 21.363654104294636, 28.042259052525786},
        {126.48828804201935, -9.3538297814919904, 126.48828804201935, -9.3538297814919886},
        {-28.013391632600275, 51.933375816383247, -28.013391632600275, 51.933375816383254},
        {-151.74223734645199, 18.49896431049056, -151.74223734645199, 18.498964310490564},
        {-2.4138897807045367, 3.2115681054241763, -2.4138897807045367, 3.2115681054241767},
        {63.981253758131118, -9.1296708469571488, 63.981253758131118, -9.129670846957147},
        {23.926414394613012, 10.810888187304219, 23.926414394613015, 10.810888187304219},
        {-19.444214367717041, 44.237831223770016, -19.444214367717038, 44.237831223770016},
        {44.328575228511454, 10.314665146129517, 44.328575228511454, 10.314665146129519},
        {-117.73000841037863, 23.815030637941604, -117.73000841037863, 23.815030637941607},
        {-32.961727911459256, -5.1559963733143928, -32.961727911459256, -5.1559963733143919},
        {26.208881586356945, -19.62342513362767, 26.208881586356949, -19.62342513362767},
        {138.65735572361541, -13.935231240056837, 138.65735572361541, -13.935231240056835},
        {22.18802917724873, -13.917293203371358, 22.18802917724873, -13.917293203371356},
        {-26.277265415049442, 9.1072484924240662, -26.277265415049442, 9.107248492424068},
        {-11.41420599807827, 51.946646934715517, -11.414205998078268, 51.946646934715517},
        {161.8293054720103, -27.464755679933994, 161.8293054720103, -27.46475567993399},
        {161.46575762864293, -26.474978469492694, 161.46575762864293, -26.47497846949269},
        {163.18526672690237, -12.41953636514808, 163.18526672690237, -12.419536365148078},
        {-135.60290299067785, -18.591647681416781, -135.60290299067785, -18.591647681416777},
        {41.988764325212969, -23.897601297149549, 41.988764325212976, -23.897601297149549},
        {93.932100758317716, 18.065053956509043, 93.932100758317716, 18.065053956509047},
        {-32.850269793906556, 33.748682144321776, -32.850269793906548, 33.748682144321776},
        {-6.6199690072526547, -6.5810436726186889, -6.6199690072526538, -6.5810436726186889},
        {-33.26070103386035, 12.813605392537188, -33.26070103386035, 12.813605392537189},
        {46.260894946764154, -18.424792787870778, 46.260894946764154, -18.424792787870775},
        {58.273127814285019, 5.2067547859560488, 58.273127814285019, 5.2067547859560497},
        {-6.4252881758072018, 42.444950766052813, -6.4252881758072018, 42.44495076605282},
        {23.580591317388134, -21.259239993916921, 23.580591317388137, -21.259239993916921},
        {69.476957471287932, 9.6563118584951759, 69.476957471287932, 9.6563118584951777},
        {-8.8123728431770267, -51.487252250116391, -8.812372843177025, -51.487252250116391},
        {99.686648162838623, -15.380031252253033, 99.686648162838623, -15.380031252253032},
        {-7.7467576913785248, 41.10268338094162, -7.7467576913785248, 41.102683380941627},
        {52.009796765443056, 20.76514446741993, 52.009796765443063, 20.76514446741993},
        {-76.850399690073985, -39.156714564824178, -76.850399690073985, -39.156714564824171},
        {-151.00505539707615, 38.068510988033459, -151.00505539707615, 38.068510988033466},
        {-166.37331647280328, 24.848901252062532, -166.37331647280328, 24.848901252062536},
        {-8.2013391948478382, 29.738590334116346, -8.2013391948478365, 29.738590334116346},
        {-113.14337711350736, -13.167968062577998, -113.14337711350736, -13.167968062577996},
        {19.473835915581226, 16.287487158242751, 19.47383591558123, 16.287487158242751},
        {-36.56161561457187, -9.1588898826562772, -36.56161561457187, -9.1588898826562755},
        {-53.523649483349502, -51.723637997244019, -53.523649483349494, -51.723637997244019},
        {137.06027819900837, -10.516609108898226, 137.06027819900837, -10.516609108898225},
        {-13.276331298489165, -26.678780544688205, -13.276331298489163, -26.678780544688205},
        {140.63761836590533, -12.263279818563081, 140.63761836590533, -12.263279818563079},
        {-45.384055828645501, -13.383977113622848, -45.384055828645494, -13.383977113622848},
        {-103.69471644126351, -41.054190987911923, -103.69471644126351, -41.054190987911916},
        {10.331185251375214, 8.4131589954488657, 10.331185251375214, 8.4131589954488675},
        {11.347100155747651, -1.2629931151790075, 11.347100155747652, -1.2629931151790075},
        {-21.588294290037318, -50.888515039085362, -21.588294290037318, -50.888515039085355},
        {103.2882404357575, 28.38240948475373, 103.2882404357575, 28.382409484753733},
        {13.524365644017532, 19.793554838962464, 13.524365644017534, 19.793554838962464},
        {-55.789955345958319, -21.315645975365427, -55.789955345958319, -21.315645975365424},
        {-151.41252537692114, -23.860171571442848, -151.41252537692114, -23.860171571442844},
        {44.695769985105272, 33.158451536083163, 44.695769985105279, 33.158451536083163},
        {46.030619357376935, -8.4225570161228749, 46.030619357376935, -8.4225570161228731},
        {-42.389643781779483, -25.790331029533444, -42.389643781779483, -25.790331029533441},
        {-103.57414412549547, -37.538659794530055, -103.57414412549547, -37.538659794530048},
        {57.910566700046772, -16.898476853029674, 57.910566700046772, -16.898476853029671},
        {-77.748408456463167, -21.669689897141964, -77.748408456463167, -21.669689897141961},
        {-81.071351443513393, -49.020967296480009, -81.071351443513393, -49.020967296480002},
        {119.53182658438004, 12.345631621685772, 119.53182658438004, 12.345631621685774},
        {23.085255704967274, 14.887264189755978, 23.085255704967274, 14.88726418975598},
        {-7.0597691228783273, 18.539752245583355, -7.0597691228783264, 18.539752245583355},
        {-166.93239791049018, -8.634248161548749, -166.93239791049018, -8.6342481615487472},
        {19.375340601724254, -26.705042877142855, 19.375340601724258, -26.705042877142855},
        {-124.66420659031337, -19.943974474339647, -124.66420659031337, -19.943974474339644},
        {141.14880867672514, 30.246344136685074, 141.14880867672514, 30.246344136685078},
        {-9.1795627123510961, 6.4600929443384398, -9.1795627123510943, 6.4600929443384398},
        {32.543271711662577, -42.986424122610856, 32.543271711662584, -42.986424122610856},
        {-68.616953116551002, 22.984049433242191, -68.616953116551002, 22.984049433242195},
        {-68.050298162898059, 9.0575696078781256, -68.050298162898059, 9.0575696078781274},
        {75.06342692745342, 8.125846908534113, 75.06342692745342, 8.1258469085341147},
        {9.9083927416101538, 33.781937762916044, 9.9083927416101538, 33.781937762916051},
        {16.241037940833078, 57.597413221876387, 16.241037940833081, 57.597413221876387},
        {-38.557191070344246, -11.813675063086041, -38.557191070344246, -11.813675063086039},
        {174.51553051513898, -17.759820867165697, 174.51553051513898, -17.759820867165693},
        {19.049796244716653, -15.502107666276952, 19.049796244716653, -15.502107666276951},
        {-115.88834682439887, -20.375521619328829, -115.88834682439887, -20.375521619328826},
        {13.496967968772221, 66.323731718165376, 13.496967968772223, 66.323731718165376},
        {-22.194470478068816, 31.020018096354477, -22.194470478068816, 31.02001809635448},
        {166.1096555553716, -24.093416608248273, 166.1096555553716, -24.093416608248269},
        {-45.911404861652812, 8.5740875290602538, -45.911404861652812, 8.5740875290602556},
        {-21.681772880271531, 6.6384607020440551, -21.681772880271531, 6.638460702044056},
        {-65.119204500006759, 27.563882134600341, -65.119204500006759, 27.563882134600345},
        {-159.16175591354536, 12.631184355079416, -159.16175591354536, 12.631184355079418},
        {-16.777244104619616, -6.08097086385831, -16.777244104619612, -6.08097086385831},
        {51.196599512659859, 85.737313440807824, 51.196599512659866, 85.737313440807824},
        {-25.709623488848649, 52.771271846892844, -25.709623488848649, 52.771271846892851},
        {-85.142777155756676, -10.365472053255122, -85.142777155756676, -10.36547205325512},
        {-154.18835997630345, -31.556973586226324, -154.18835997630345, -31.55697358622632},
        {-52.857415916033226, -43.485336277537073, -52.857415916033226, -43.485336277537066},
        {-41.921487362346355, 58.613179152149847, -41.921487362346348, 58.613179152149847},
        {43.503003345597421, -0.64543927860452044, 43.503003345597428, -0.64543927860452044},
        {-103.20271803014451, -10.00255040581742, -103.20271803014451, -10.002550405817418},
        {-38.127931415293332, 73.017800491059873, -38.127931415293325, 73.017800491059873},
        {-39.611857106634076, 84.329588048416525, -39.611857106634069, 84.329588048416525},
        {10.037461759046206, 13.068090704656544, 10.037461759046206, 13.068090704656546},
        {57.045901294561915, 42.882528546051603, 57.045901294561922, 42.882528546051603},
        {134.85563357019527, 8.1816750013605439, 134.85563357019527, 8.1816750013605457},
        {-157.14389614161936, 12.220150496985912, -157.14389614161936, 12.220150496985914},
        {-2.4670214829884429, 9.4648357448560496, -2.4670214829884429, 9.4648357448560514},
        {131.4337286657732, 8.2189365756595674, 131.4337286657732, 8.2189365756595691},
        {-95.51935579416066, 23.79403255504738, -95.51935579416066, 23.794032555047384},
        {-39.331303535376847, -25.692024280827805, -39.33130353537684, -25.692024280827805},
        {-33.247264620976914, -8.8658969094553388, -33.247264620976914, -8.865896909455337},
        {-123.19982652092813, -10.09019728415041, -123.19982652092813, -10.090197284150408},
        {118.61741267583605, 11.876494519722915, 118.61741267583605, 11.876494519722916},
        {-22.173002037760774, -7.067568738041027, -22.173002037760771, -7.067568738041027},
        {-53.157241085505937, 63.832792106564511, -53.15724108550593, 63.832792106564511},
        {-0.27437479353160654, 8.7948995390706877, -0.27437479353160654, 8.7948995390706894},
        {-46.307153312547143, -7.7999996640955462, -46.307153312547143, -7.7999996640955453},
        {-21.768729273800467, -63.427667499930372, -21.768729273800464, -63.427667499930372},
        {141.43992057955205, 9.3107597736175798, 141.43992057955205, 9.3107597736175816},
        {-123.07982227503794, -27.512585185167843, -123.07982227503794, -27.51258518516784},
        {-11.114296521704258, -61.940581558073944, -11.114296521704256, -61.940581558073944},
        {-118.64341643284099, 12.813688519879637, -118.64341643284099, 12.813688519879639},
        {149.10419607450467, 11.934789307926582, 149.10419607450467, 11.934789307926584},
        {4.2113282140704911, 21.190436976262504, 4.2113282140704911, 21.190436976262507},
        {24.133116538046558, -12.381088212768464, 24.133116538046558, -12.381088212768462},
        {-15.781185839030332, -9.2111512763284455, -15.781185839030332, -9.2111512763284438},
        {98.67006686888331, -26.99131013919547, 98.67006686888331, -26.991310139195466},
        {-58.1850759732461, -11.728930462491508, -58.1850759732461, -11.728930462491507},
        {153.63350768888557, 24.900159646598155, 153.63350768888557, 24.900159646598159},
        {-31.695530743574857, -50.812116477328786, -31.695530743574853, -50.812116477328786},
        {-11.986959552316389, 55.832250541314274, -11.986959552316387, 55.832250541314274},
        {4.5215513225266335, -20.438847452712956, 4.5215513225266344, -20.438847452712956},
        {-86.259519967511778, -11.852756092552074, -86.259519967511778, -11.852756092552072},
        {81.643453892298439, 53.123269221920992, 81.643453892298439, 53.123269221920999},
        {54.949611041665335, 35.536195824864635, 54.949611041665342, 35.536195824864635},
        {-49.112370568605819, -10.092415001587302, -49.112370568605819, -10.0924150015873},
        {-108.08479145022591, -15.786509686226704, -108.08479145022591, -15.786509686226703},
        {9.1942477345835663, 19.455060087237921, 9.194247734583568, 19.455060087237921},
        {175.40812595032278, 9.9778741212932012, 175.40812595032278, 9.9778741212932029},
        {-22.069870411888537, -10.979810013064085, -22.069870411888537, -10.979810013064084},
        {6.0495296474468008, 50.836683929954631, 6.0495296474468017, 50.836683929954631},
        {-48.896037684370945, 50.686305035027814, -48.896037684370938, 50.686305035027814},
        {83.386597860324727, -15.969648582662497, 83.386597860324727, -15.969648582662495},
        {0.95695944403608735, 56.254798532830485, 0.95695944403608735, 56.254798532830492},
        {-11.33685908283891, -10.305763221276582, -11.336859082838908, -10.305763221276582},
        {71.95519887974956, -10.602803876691883, 71.95519887974956, -10.602803876691882},
        {179.59037272170761, -7.4227620012376807, 179.59037272170761, -7.4227620012376798},
        {98.663763863611678, 8.7759839510474116, 98.663763863611678, 8.7759839510474134},
        {-131.22576234940564, -27.056730954659251, -131.22576234940564, -27.056730954659248},
        {-153.96985020796072, -16.01371653396593, -153.96985020796072, -16.013716533965926},
        {-178.60463836377861, 7.098705132172169, -178.60463836377861, 7.0987051321721699},
        {-16.918719439172339, -59.380091529353471, -16.918719439172335, -59.380091529353471},
        {-151.10535349301034, 55.808046659800844, -151.10535349301034, 55.808046659800851},
        {-141.8652599526466, 9.2125105805873666, -141.8652599526466, 9.2125105805873684},
        {-43.603525180660597, 16.300619297900052, -43.603525180660597, 16.300619297900056},
        {-63.209614596714808, 10.600187017370008, -63.209614596714808, 10.600187017370009},
        {-24.0290628202061, -60.991017151961948, -24.029062820206097, -60.991017151961948},
        {102.90729673300035, -36.455263019327781, 102.90729673300035, -36.455263019327774},
        {-10.03709515927039, -15.625183891036823, -10.037095159270388, -15.625183891036823},
        {-33.498684609647761, 20.496539811712509, -33.498684609647754, 20.496539811712509},
        {153.8130116324067, 13.045262271887456, 153.8130116324067, 13.045262271887458},
        {57.971656051003166, -15.523976728535327, 57.971656051003166, -15.523976728535326},
        {-173.68694781368259, 16.278389351244751, -173.68694781368259, 16.278389351244755},
        {14.113474658009547, -24.73823051490049, 14.113474658009549, -24.73823051490049},
        {-102.78399363158671, -32.009401548617689, -102.78399363158671, -32.009401548617681},
        {141.17648459676022, 45.770770232732247, 141.17648459676022, 45.770770232732254},
        {172.74594794901017, 19.728757583057231, 172.74594794901017, 19.728757583057234},
        {-100.49685816403525, -9.6581859013431117, -100.49685816403525, -9.6581859013431099},
        {-140.60932633222819, 23.663820670069924, -140.60932633222819, 23.663820670069928},
        {91.192603951715341, 11.268681488219773, 91.192603951715341, 11.268681488219775},
        {136.99849690406123, -9.4799215954363039, 136.99849690406123, -9.4799215954363021},
        {-21.703147964155541, 15.808906907129579, -21.703147964155541, 15.808906907129581},
        {-12.258569147849098, -8.8052133153702474, -12.258569147849098, -8.8052133153702457},
        {158.20668157111945, 13.465047691381006, 158.20668157111945, 13.465047691381008},
        {23.445590690855063, -29.047462333568454, 23.445590690855067, -29.047462333568454},
        {11.43284246780507, 15.149520586162122, 11.43284246780507, 15.149520586162124},
        {7.0420185199273222, -54.463759354074256, 7.0420185199273231, -54.463759354074256},
        {-52.052677893203985, 6.1888725579532018, -52.052677893203985, 6.1888725579532027},
        {47.791563531627418, 60.497368687548317, 47.791563531627425, 60.497368687548317},
        {-57.387001828180061, -14.024878123822774, -57.387001828180061, -14.024878123822772},
        {74.490277679661219, -8.345226731489527, 74.490277679661219, -8.3452267314895252},
        {17.815647801333377, -76.185482394957745, 17.81564780133338, -76.185482394957745},
        {14.703386738342029, -37.724175788030315, 14.703386738342029, -37.724175788030308},
        {-31.666203752560641, -11.30861614136251, -31.666203752560641, -11.308616141362508},
        {65.963474087628839, 8.3213574258144813, 65.963474087628839, 8.3213574258144831},
        {38.11417054290677, 20.369824148346794, 38.11417054290677, 20.369824148346797},
        {-56.531998212833074, -8.2142394219164974, -56.531998212833066, -8.2142394219164974},
        {-56.531998212833074, -8.2142394219164974, -56.531998212833074, -8.2142394219164956},
        {58.752264319403608, -17.939205926946851, 58.752264319403608, -17.939205926946848},
        {-74.901137042906683, -31.496992886779726, -74.901137042906683, -31.496992886779722},
        {34.396485029048641, 40.502643743173259, 34.396485029048641, 40.502643743173266},
        {23.097830220605296, -70.21640930148692, 23.097830220605299, -70.21640930148692},
        {50.83512482891534, -53.961401476062619, 50.835124828915347, -53.961401476062619},
        {-33.251153476440486, 12.888136026341289, -33.251153476440486, 12.888136026341291},
        {108.62402988186365, 10.203220935545833, 108.62402988186365, 10.203220935545835},
        {-23.2813253836784, 22.751758996674994, -23.2813253836784, 22.751758996674997},
        {45.924448871026243, 29.308971200417623, 45.92444887102625, 29.308971200417623},
        {137.47316151728879, 52.13899905440698, 137.47316151728879, 52.138999054406987},
        {-19.844451478221153, -44.660649498536088, -19.844451478221149, -44.660649498536088},
        {143.94702093415847, -18.228552309566933, 143.94702093415847, -18.228552309566929},
        {-93.699946543237729, 13.252167515573777, -93.699946543237729, 13.252167515573779},
        {-26.285753003256104, -19.378513982674583, -26.285753003256104, -19.37851398267458},
        {15.432522305920852, -45.426128727931911, 15.432522305920854, -45.426128727931911},
        {-85.004924503899218, 30.715680765735264, -85.004924503899218, 30.715680765735268},
        {-119.34515870595312, -17.711461052375579, -119.34515870595312, -17.711461052375576},
        {-7.3963834331574159, -18.560129939245122, -7.3963834331574159, -18.560129939245119},
        {108.45091406475949, -13.778916867001186, 108.45091406475949, -13.778916867001184},
        {22.631981612414769, 84.813015791146768, 22.631981612414773, 84.813015791146768},
        {-170.93863008062695, 7.2554436568984055, -170.93863008062695, 7.2554436568984064},
        {-138.34544343966158, 11.516082317106115, -138.34544343966158, 11.516082317106116},
        {179.28060439800404, 41.972273340371267, 179.28060439800404, 41.972273340371274},
        {-129.23804603861225, 7.6782388901277994, -129.23804603861225, 7.6782388901278003},
        {39.537577142754486, 82.44653888347726, 39.537577142754493, 82.44653888347726},
        {31.418060814401706, -34.608824754087252, 31.418060814401709, -34.608824754087252},
        {-42.852359504285374, -43.174100100268731, -42.852359504285374, -43.174100100268724},
        {44.611457184612334, -70.254722264636484, 44.611457184612341, -70.254722264636484},
        {-127.08778498632252, 23.820706192088579, -127.08778498632252, 23.820706192088583},
        {142.21578150613684, 26.184525253220691, 142.21578150613684, 26.184525253220695},
        {-148.67287868831755, 40.03819103771054, -148.67287868831755, 40.038191037710547},
        {135.73313079730266, 10.543595093706916, 135.73313079730266, 10.543595093706918},
        {-28.381089757079707, -7.423993275950588, -28.381089757079703, -7.423993275950588},
        {-172.79697800132323, 16.814541190166022, -172.79697800132323, 16.814541190166025},
        {142.82873099296697, 3.4884230131203808, 142.82873099296697, 3.4884230131203813},
        {164.45352579781269, 9.991869521163256, 164.45352579781269, 9.9918695211632578},
        {-19.297334495604435, -19.373865266007932, -19.297334495604435, -19.373865266007929},
        {94.686989357121519, -7.5537287240617648, 94.686989357121519, -7.5537287240617639},
        {-67.46261704408542, 16.807855156714336, -67.46261704408542, 16.807855156714339},
        {-85.590795777407834, 22.919749683302982, -85.590795777407834, 22.919749683302985},
        {27.221506379445806, -30.22247830719515, 27.22150637944581, -30.22247830719515},
        {-105.23069780339129, -36.84063654464746, -105.23069780339129, -36.840636544647452},
        {-18.459671071821038, -89.454043585304746, -18.459671071821035, -89.454043585304746},
        {-150.20242220126934, 11.812891083681127, -150.20242220126934, 11.812891083681128},
        {40.433461108738925, -45.950177503884703, 40.433461108738932, -45.950177503884703},
        {-32.266204259572127, -26.691075680124694, -32.266204259572127, -26.69107568012469},
        {47.975817144635698, 10.43727917457041, 47.975817144635698, 10.437279174570412},
        {10.352218361382167, 22.235583717450396, 10.352218361382169, 22.235583717450396},
        {-29.002799486824273, 17.257767985990167, -29.002799486824273, 17.257767985990171},
        {82.499913941376164, 9.2693501717855789, 82.499913941376164, 9.2693501717855806},
        {-169.68860120624339, -11.959016917429825, -169.68860120624339, -11.959016917429823},
        {7.040755146092974, -69.213042862479526, 7.0407551460929749, -69.213042862479526},
        {-7.2792318741513835, -37.687477941275297, -7.2792318741513826, -37.687477941275297},
        {-54.882576555749253, 48.48659927527244, -54.882576555749246, 48.48659927527244},
        {8.1665023885006995, 9.9602265799679692, 8.1665023885006995, 9.960226579967971},
        {-57.894331406384353, -18.667282174292872, -57.894331406384353, -18.667282174292868},
        {177.22901058239671, -24.731152599093118, 177.22901058239671, -24.731152599093114},
        {38.563192927021156, 12.726701384056231, 38.563192927021156, 12.726701384056232},
        {-16.801398916393413, 38.823245759767779, -16.80139891639341, 38.823245759767779},
        {-51.286288345187039, 49.0679617768307, -51.286288345187032, 49.0679617768307},
        {-101.02010650134787, -16.50063751357801, -101.02010650134787, -16.500637513578006},
        {-54.304343118831028, 19.869513192508677, -54.304343118831028, 19.869513192508681},
        {-86.880027626892456, -37.868993248501774, -86.880027626892456, -37.868993248501766},
        {-2.5002876529338636, -28.854379811885302, -2.5002876529338631, -28.854379811885302},
        {17.091431203087531, -80.031503764150685, 17.091431203087534, -80.031503764150685},
        {-56.274838361761809, 8.3564841865517376, -56.274838361761809, 8.3564841865517394},
        {-27.442093583923992, -8.6397676186250951, -27.442093583923992, -8.6397676186250933},
        {20.179617202656324, -20.736146224983507, 20.179617202656328, -20.736146224983507},
        {27.690164945832699, -39.108751350157043, 27.690164945832699, -39.108751350157036},
        {19.109161735091085, -0.41333698612643133, 19.109161735091089, -0.41333698612643133},
        {-15.794485028878274, -9.2854105969596592, -15.794485028878274, -9.2854105969596574},
        {6.4511071396892961, 49.800449388780876, 6.4511071396892961, 49.800449388780883},
        {59.339878032780135, 10.578971028073102, 59.339878032780135, 10.578971028073104},
        {110.79250312959023, -33.05550512767725, 110.79250312959023, -33.055505127677243},
        {118.80852022587186, -13.224568240167272, 118.80852022587186, -13.224568240167271},
        {13.28670632098277, -34.288604291774348, 13.286706320982772, -34.288604291774348},
        {55.807533299123676, -23.805469900739354, 55.807533299123676, -23.80546990073935},
        {-32.856937018105207, -7.2630588936026843, -32.856937018105199, -7.2630588936026843},
        {88.793582496794812, -36.98850433683733, 88.793582496794812, -36.988504336837323},
        {-4.7401693074321312, 21.639681390315005, -4.7401693074321312, 21.639681390315008},
        {-8.2990910731184382, -12.853222555183322, -8.2990910731184364, -12.853222555183322},
        {-14.306810857950001, -11.961136221711332, -14.306810857950001, -11.96113622171133},
        {-37.455991174103147, -84.861394077753602, -37.45599117410314, -84.861394077753602},
        {176.34181453922864, 7.3458368064456732, 176.34181453922864, 7.345836806445674},
        {-154.03452302995638, -36.937741979047466, -154.03452302995638, -36.937741979047459},
        {-121.75791424877453, -53.149206108501886, -121.75791424877453, -53.149206108501879},
        {22.514632068488119, -12.343514866592646, 22.514632068488119, -12.343514866592644},
        {50.768088060079165, 8.7149679065348131, 50.768088060079165, 8.7149679065348149},
        {101.80530195165048, -24.34973824280446, 101.80530195165048, -24.349738242804456},
        {41.896218466881351, -44.110550958898273, 41.896218466881351, -44.110550958898266},
        {108.03174704003301, 7.124602816599845, 108.03174704003301, 7.1246028165998458},
        {54.503346827486595, -40.54334856009131, 54.503346827486602, -40.54334856009131},
        {12.999564082240024, -7.0030916939672334, 12.999564082240024, -7.0030916939672325},
        {7.3082731443861038, 32.050968869735016, 7.3082731443861046, 32.050968869735016},
        {136.7408893675275, -15.02074370350131, 136.7408893675275, -15.020743703501308},
        {-11.912720499766149, -51.33753448328968, -11.912720499766147, -51.33753448328968},
        {116.24992697194874, -9.1504838332432517, 116.24992697194874, -9.1504838332432499},
        {102.81305404988117, -20.682346689296942, 102.81305404988117, -20.682346689296939},
        {2.8441815727143904, -19.441974876380037, 2.8441815727143904, -19.441974876380034},
        {110.38044071485207, -36.933250466246101, 110.38044071485207, -36.933250466246093},
        {35.643388289392611, 17.057174098347073, 35.643388289392618, 17.057174098347073},
        {117.25860671070511, -9.3349516959632641, 117.25860671070511, -9.3349516959632624},
        {67.48981180611554, -11.774542186328748, 67.48981180611554, -11.774542186328746},
        {49.859823488714113, 9.7591525507439805, 49.859823488714113, 9.7591525507439822},
        {-6.4309632215550945, 74.129222478983522, -6.4309632215550936, 74.129222478983522},
        {11.358896763232419, 7.1189151148988667, 11.358896763232421, 7.1189151148988667},
        {-106.54114379508587, 23.99493823025648, -106.54114379508587, 23.994938230256484},
        {-60.231932855014357, -23.333079081596651, -60.231932855014357, -23.333079081596647},
        {127.68670636544167, 12.674148805508811, 127.68670636544167, 12.674148805508812},
        {-52.665156426386197, 42.72447158657711, -52.66515642638619, 42.72447158657711},
        {-106.57696651456209, -9.5737583474050272, -106.57696651456209, -9.5737583474050254},
        {98.323393015990916, 14.056746678835598, 98.323393015990916, 14.0567466788356},
        {-27.578027564462985, -19.937034901247479, -27.578027564462985, -19.937034901247475},
        {-9.6042164541946953, -35.380416472749822, -9.6042164541946935, -35.380416472749822},
        {51.03703302616934, -39.506795637496488, 51.037033026169347, -39.506795637496488},
        {161.56847770224323, -10.859002244544985, 161.56847770224323, -10.859002244544984},
        {-74.414372845233075, -9.4841496753934909, -74.414372845233075, -9.4841496753934891},
        {74.071886862780261, 8.7430938479865041, 74.071886862780261, 8.7430938479865059},
        {-23.005195799182736, 60.976894270283111, -23.005195799182733, 60.976894270283111},
        {135.25777910562465, 16.679639151267008, 135.25777910562465, 16.679639151267011},
        {-169.32463916025694, 11.473872141822103, -169.32463916025694, 11.473872141822104},
        {-106.98570000735958, -9.509442405351928, -106.98570000735958, -9.5094424053519262},
        {-103.37467541276881, -19.647402888038137, -103.37467541276881, -19.647402888038133},
        {-24.483770185735388, -43.159206284608274, -24.483770185735384, -43.159206284608274},
        {178.15830989190886, -24.993368517539079, 178.15830989190886, -24.993368517539075},
        {-61.143041620149468, -34.163898422719768, -61.143041620149468, -34.163898422719761},
        {-18.056448594455066, 20.620545676086309, -18.056448594455063, 20.620545676086309},
        {11.869845958289996, 78.110591293386975, 11.869845958289998, 78.110591293386975},
        {2.4527713193626965, -10.148054205832796, 2.4527713193626965, -10.148054205832794},
        {10.598793300173636, 14.282412823624025, 10.598793300173636, 14.282412823624027},
        {-67.140304359947606, 13.226518767432086, -67.140304359947606, 13.226518767432088},
        {9.0035399221411829, -5.083616061554876, 9.0035399221411847, -5.083616061554876},
        {-178.77275969726696, 54.070892412227941, -178.77275969726696, 54.070892412227948},
        {36.481798032108422, -18.979615664386777, 36.481798032108422, -18.979615664386774},
        {-25.212852710350269, 4.2320639598741234, -25.212852710350266, 4.2320639598741234},
        {17.124080676660903, 42.430489602089864, 17.124080676660906, 42.430489602089864},
        {-66.943663748375172, -11.082004580765105, -66.943663748375172, -11.082004580765103},
        {10.236307822228778, -27.610809639874027, 10.236307822228778, -27.610809639874024},
        {53.847096210534303, -19.03897613994193, 53.847096210534303, -19.038976139941926},
        {32.975607927933673, -64.737685825260542, 32.97560792793368, -64.737685825260542},
        {24.818546461478384, -47.166382744416133, 24.818546461478387, -47.166382744416133},
        {152.00223336912805, 56.312518929205332, 152.00223336912805, 56.312518929205339},
        {81.186671556438554, -10.152015393379731, 81.186671556438554, -10.152015393379729},
        {55.284485876526318, 12.287119220408943, 55.284485876526325, 12.287119220408943},
        {124.2543079131803, 31.051015942516372, 124.2543079131803, 31.051015942516376},
        {-25.677860681994844, -58.59588417702998, -25.67786068199484, -58.59588417702998},
        {9.0945137752664742, -53.356106819362893, 9.094513775266476, -53.356106819362893},
        {-54.82827958415966, -10.506554244001055, -54.82827958415966, -10.506554244001054},
        {68.651497851811484, -16.808849898800798, 68.651497851811484, -16.808849898800794},
        {75.408995121015963, -49.921861360266035, 75.408995121015963, -49.921861360266028},
        {-60.245461283203248, -13.286501104968039, -60.245461283203248, -13.286501104968037},
        {-11.334089880713062, -68.507584008625031, -11.33408988071306, -68.507584008625031},
        {-85.191167398001909, 19.42383997990272, -85.191167398001909, 19.423839979902723},
        {22.533598812576379, -2.1973094479443231, 22.533598812576383, -2.1973094479443231},
        {-53.669553008254226, -41.961973298295064, -53.669553008254219, -41.961973298295064},
        {-22.623237737054048, -80.447160987482363, -22.623237737054044, -80.447160987482363},
        {47.31511098136815, -44.660877040151718, 47.315110981368157, -44.660877040151718},
        {91.340447576827273, -27.063456886773231, 91.340447576827273, -27.063456886773228},
        {-70.460096591312052, -27.196384442311434, -70.460096591312052, -27.19638444231143},
        {9.5849562131254764, 21.558846017130094, 9.5849562131254782, 21.558846017130094},
        {16.136130280194546, 40.986451629119607, 16.13613028019455, 40.986451629119607},
        {79.784980677067736, -13.195694287027152, 79.784980677067736, -13.19569428702715},
        {-47.093899661120361, -72.112706291490994, -47.093899661120354, -72.112706291490994},
        {-53.862230152512311, -72.629040637738186, -53.862230152512304, -72.629040637738186},
        {-18.127083612338442, 13.228350344002497, -18.127083612338442, 13.228350344002498},
        {-99.547136259446916, 26.216674399752186, -99.547136259446916, 26.21667439975219},
        {-90.132081961933153, 10.031115882680059, -90.132081961933153, 10.03111588268006},
        {-32.829228539438496, 20.447455902589098, -32.829228539438496, 20.447455902589102},
        {-36.829695306123327, 9.6087219616239175, -36.829695306123327, 9.6087219616239192},
        {39.890681986092972, 24.9286621844129, 39.890681986092979, 24.9286621844129},
        {-119.32129573725, -37.389556241176386, -119.32129573725, -37.389556241176379},
        {-110.65057556449904, 51.350106808978992, -110.65057556449904, 51.350106808979},
        {158.28624428268603, 11.45158115190036, 158.28624428268603, 11.451581151900362},
        {-17.246704401391629, -1.7020917025189874, -17.246704401391625, -1.7020917025189874},
        {23.806664093841199, -28.115773067427547, 23.806664093841199, -28.115773067427543},
        {-12.455726379505844, -84.687509142886682, -12.455726379505842, -84.687509142886682},
        {34.220768579859758, 32.978438566070679, 34.220768579859765, 32.978438566070679},
        {29.500431486939554, 17.591856055888211, 29.500431486939554, 17.591856055888215},
        {173.34886081637774, -24.351781414661442, 173.34886081637774, -24.351781414661438},
        {-46.981071217385534, 21.68155901496111, -46.981071217385534, 21.681559014961113},
        {27.095850930487877, -21.025998099309682, 27.095850930487881, -21.025998099309682},
        {-14.595551087529362, -16.630380740724842, -14.59555108752936, -16.630380740724842},
        {-164.93137178681999, 7.5064483875652694, -164.93137178681999, 7.5064483875652703},
        {-144.26451444278132, -18.642269359940649, -144.26451444278132, -18.642269359940645},
        {52.998596217768593, -75.295148408280156, 52.998596217768601, -75.295148408280156},
        {78.882180755433851, 14.245119969163181, 78.882180755433851, 14.245119969163182},
        {164.01559557176267, 26.562428430543854, 164.01559557176267, 26.562428430543857},
        {5.7164940926395191, -11.424666268619536, 5.7164940926395191, -11.424666268619534},
        {-126.39817666456831, -17.303683049701586, -126.39817666456831, -17.303683049701583},
        {-9.622719307594533, 40.995487914304043, -9.6227193075945312, 40.995487914304043},
        {14.977875137710694, -36.975105793138162, 14.977875137710694, -36.975105793138155},
        {119.15550736108722, -22.168120062312553, 119.15550736108722, -22.16812006231255},
        {145.00724866424181, 56.704785730022941, 145.00724866424181, 56.704785730022948},
        {99.854262873993775, -8.2919057240209728, 99.854262873993775, -8.291905724020971},
        {26.031843368324566, 47.558283224065136, 26.031843368324569, 47.558283224065136},
        {-93.703200325001433, 26.926078007942984, -93.703200325001433, 26.926078007942987},
        {118.62752254785144, -19.968258233550493, 118.62752254785144, -19.96825823355049},
        {145.15712974483296, 56.149044646281489, 145.15712974483296, 56.149044646281496},
        {37.572836780501909, -12.370059835747259, 37.572836780501909, -12.370059835747258},
        {8.2249645424716924, -35.989027496797327, 8.2249645424716942, -35.989027496797327},
        {-92.977257664003019, -18.158475433362991, -92.977257664003019, -18.158475433362987},
        {10.112885912567403, -6.0698816724263178, 10.112885912567405, -6.0698816724263178},
        {28.576312085508754, 17.13308374499621, 28.576312085508754, 17.133083744996213},
        {130.5879920562769, -12.408079961250213, 130.5879920562769, -12.408079961250211},
        {46.276310385585276, 5.6698976598015189, 46.276310385585276, 5.6698976598015198},
        {-15.431117828383083, 56.148196534228582, -15.431117828383083, 56.148196534228589},
        {-29.080553329369565, 18.061522729623743, -29.080553329369565, 18.061522729623746},
        {94.95552406046184, 22.679456157715492, 94.95552406046184, 22.679456157715496},
        {71.572342882500692, 9.6442815787346383, 71.572342882500692, 9.6442815787346401},
        {171.70028081900352, 47.381062639056196, 171.70028081900352, 47.381062639056204},
        {-176.1716525070365, 12.420046154933505, -176.1716525070365, 12.420046154933507},
        {6.4250705879612413, 28.454329033234835, 6.4250705879612413, 28.454329033234838},
        {-171.02992244031608, 20.296791608874798, -171.02992244031608, 20.296791608874802},
        {54.725185275333516, -9.9693885240948568, 54.725185275333516, -9.969388524094855},
        {-19.080959448691733, -87.043803532793461, -19.080959448691729, -87.043803532793461},
        {-103.41142547751504, -13.736093988221555, -103.41142547751504, -13.736093988221553},
        {-175.67164671408659, -11.12051564111923, -175.67164671408659, -11.120515641119228},
        {-43.725397490732206, 9.0382092981221298, -43.725397490732206, 9.0382092981221316},
        {83.497634867433632, 56.625947003455671, 83.497634867433632, 56.625947003455678},
        {90.388166190541, -50.8394661041973, 90.388166190541, -50.839466104197292},
        {139.60924996092129, -8.5872755041146522, 139.60924996092129, -8.5872755041146505},
        {71.513043875751265, 44.574307228718396, 71.513043875751265, 44.574307228718403},
        {20.636209855557588, 56.611183867247753, 20.636209855557588, 56.61118386724776},
        {-51.865183674277056, 16.213940200041062, -51.865183674277056, 16.213940200041066},
        {-41.652067799430981, -14.919280333935291, -41.652067799430981, -14.919280333935289},
        {-32.507847441323086, -51.632809883668436, -32.507847441323079, -51.632809883668436},
        {-64.919370706160208, 27.498282719337929, -64.919370706160208, 27.498282719337933},
        {56.539983466030307, -26.658971998196161, 56.539983466030307, -26.658971998196158},
        {-14.198621657961457, -19.554634721321698, -14.198621657961457, -19.554634721321694},
        {-69.014847193832935, -13.512645660125045, -69.014847193832935, -13.512645660125044},
        {-12.911215894329583, 39.16870508038766, -12.911215894329581, 39.16870508038766},
        {145.47282533202807, 16.230719259286442, 145.47282533202807, 16.230719259286445},
        {98.346090720193473, -16.054517089550895, 98.346090720193473, -16.054517089550892},
        {146.29884375742637, 18.587045876454081, 146.29884375742637, 18.587045876454084},
        {-167.69667600898569, 28.091140204006841, -167.69667600898569, 28.091140204006845},
        {9.1079352000175753, -49.692562059418748, 9.1079352000175771, -49.692562059418748},
        {173.56016874049229, -23.913640466101267, 173.56016874049229, -23.913640466101263},
        {170.9172592344859, 19.91489914809766, 170.9172592344859, 19.914899148097664},
        {65.457601425747811, 8.4090954622755572, 65.457601425747811, 8.4090954622755589},
        {-23.107807888939085, -74.384516094026992, -23.107807888939082, -74.384516094026992},
        {10.050875593163905, -77.453504424626402, 10.050875593163907, -77.453504424626402},
        {30.364496347869526, 42.339920066264213, 30.364496347869526, 42.33992006626422},
        {132.75709034205104, -10.17296764997376, 132.75709034205104, -10.172967649973758},
        {-115.88464206452974, -11.944342229402526, -115.88464206452974, -11.944342229402524},
        {-14.312317681929859, -53.070968051822845, -14.312317681929857, -53.070968051822845},
        {7.2559032846963278, 43.917080430408916, 7.2559032846963287, 43.917080430408916},
        {13.216869026594335, -51.001259109787469, 13.216869026594336, -51.001259109787469},
        {-13.701607663520971, -80.6211774350393, -13.701607663520969, -80.6211774350393},
        {30.599065704485234, -45.150991474774628, 30.599065704485238, -45.150991474774628},
        {-12.639577398179934, -20.516516850010014, -12.639577398179933, -20.516516850010014},
        {178.98790485795544, 19.044854730156853, 178.98790485795544, 19.044854730156857},
        {-145.70047250082627, 26.110666957652235, -145.70047250082627, 26.110666957652239},
        {-72.89435711262901, -12.17061053937201, -72.89435711262901, -12.170610539372008},
        {-173.58882938178166, 21.570977069411839, -173.58882938178166, 21.570977069411843},
        {147.61769799520241, 19.528476328868603, 147.61769799520241, 19.528476328868607},
        {22.075769827425098, 12.897090604054856, 22.075769827425102, 12.897090604054856},
        {-33.364904192054659, 51.153817808610071, -33.364904192054652, 51.153817808610071},
        {-107.40878935285696, -12.895231922748081, -107.40878935285696, -12.89523192274808},
        {19.385589169462072, -78.374947184692331, 19.385589169462076, -78.374947184692331},
        {49.567898838653207, 9.2290115457537816, 49.567898838653207, 9.2290115457537834},
        {-142.05211641067791, 13.273094716608259, -142.05211641067791, 13.273094716608261},
        {-130.43967970993162, -18.464113116905132, -130.43967970993162, -18.464113116905128},
        {-101.24331457319684, -14.9208796043864, -101.24331457319684, -14.920879604386398},
        {78.286076934835265, -27.520771776090449, 78.286076934835265, -27.520771776090445},
    };
    // clang-format on

    PolygonWithCRS polygon;
    for (const auto& p : kPairs) {
        BSONObjBuilder b;
        b.append("type", "Polygon");
        BSONArrayBuilder coords(b.subarrayStart("coordinates"));
        {
            BSONArrayBuilder ring(coords.subarrayStart());
            ring.append(BSON_ARRAY(0.0 << 0.0));
            ring.append(BSON_ARRAY(p.lng1 << p.lat1));
            ring.append(BSON_ARRAY(p.lng2 << p.lat2));
            ring.append(BSON_ARRAY(0.0 << 0.0));
        }
        coords.done();
        ASSERT_OK(GeoParser::parseGeoJSONPolygon(b.obj(), false, &polygon));
    }
}

}  // namespace
