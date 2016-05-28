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

/**
 * This file contains tests for mongo/db/geo/geoparser.cpp.
 */

#include <sstream>
#include <string>

#include "mongo/db/geo/geoparser.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

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
}
}
