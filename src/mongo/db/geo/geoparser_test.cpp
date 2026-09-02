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
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <s2cellid.h>
#include <s2polygon.h>

// Wrap a BSON object to a BSON element.
#define BSON_ELT(bson) BSON("" << (bson)).firstElement()

using namespace mongo;

namespace {

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
}  // namespace
