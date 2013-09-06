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

#include <string>
#include <sstream>

#include "mongo/db/geo/geoparser.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/db/json.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

using namespace mongo;

namespace {

    TEST(GeoParser, isValidPoint) {
        ASSERT_TRUE(GeoParser::isPoint(fromjson("{'type':'Point', 'coordinates': [40, 5]}")));
        ASSERT_TRUE(GeoParser::isPoint(
            fromjson("{'type':'Point', 'coordinates': [-40.3, -5.0]}")));
        ASSERT_FALSE(GeoParser::isPoint(fromjson("{'typo':'Point', 'coordinates': [40, -5]}")));
        ASSERT_FALSE(GeoParser::isPoint(fromjson("{'type':'Point', 'coordhats': [40, -5]}")));
        ASSERT_FALSE(GeoParser::isPoint(
            fromjson("{'type':['Point'], 'coordinates': [40, -5]}")));
        ASSERT_FALSE(GeoParser::isPoint(fromjson("{'type':'Point', 'coordinates': 40}")));
        ASSERT_FALSE(GeoParser::isPoint(
            fromjson("{'type':'Point', 'coordinates': [40, -5, 7]}")));

        // Make sure lat is in range
        ASSERT_TRUE(GeoParser::isPoint(fromjson("{'type':'Point', 'coordinates': [0, 90.0]}")));
        ASSERT_TRUE(GeoParser::isPoint(fromjson("{'type':'Point', 'coordinates': [0, -90.0]}")));
        ASSERT_TRUE(GeoParser::isPoint(fromjson("{'type':'Point', 'coordinates': [180, 90.0]}")));
        ASSERT_TRUE(GeoParser::isPoint(fromjson("{'type':'Point', 'coordinates': [-180, -90.0]}")));
        ASSERT_FALSE(GeoParser::isPoint(fromjson("{'type':'Point', 'coordinates': [180.01, 90.0]}")));
        ASSERT_FALSE(GeoParser::isPoint(fromjson("{'type':'Point', 'coordinates': [-180.01, -90.0]}")));
        ASSERT_FALSE(GeoParser::isPoint(fromjson("{'type':'Point', 'coordinates': [0, 90.1]}")));
        ASSERT_FALSE(GeoParser::isPoint(fromjson("{'type':'Point', 'coordinates': [0, -90.1]}")));
    }

    TEST(GeoParser, isValidLineString) {
        ASSERT_TRUE(GeoParser::isLine(
            fromjson("{'type':'LineString', 'coordinates':[[1,2], [3,4]]}")));
        ASSERT_TRUE(GeoParser::isLine(
            fromjson("{'type':'LineString', 'coordinates':[[0,-90], [0,90]]}")));
        ASSERT_TRUE(GeoParser::isLine(
            fromjson("{'type':'LineString', 'coordinates':[[180,-90], [-180,90]]}")));
        ASSERT_FALSE(GeoParser::isLine(
            fromjson("{'type':'LineString', 'coordinates':[[180.1,-90], [-180.1,90]]}")));
        ASSERT_FALSE(GeoParser::isLine(
            fromjson("{'type':'LineString', 'coordinates':[[0,-91], [0,90]]}")));
        ASSERT_FALSE(GeoParser::isLine(
            fromjson("{'type':'LineString', 'coordinates':[[0,-90], [0,91]]}")));
        ASSERT_TRUE(GeoParser::isLine(
            fromjson("{'type':'LineString', 'coordinates':[[1,2], [3,4], [5,6]]}")));
        ASSERT_FALSE(GeoParser::isLine(
            fromjson("{'type':'LineString', 'coordinates':[[1,2]]}")));
        ASSERT_FALSE(GeoParser::isLine(
            fromjson("{'type':'LineString', 'coordinates':[['chicken','little']]}")));
        ASSERT_FALSE(GeoParser::isLine(
            fromjson("{'type':'LineString', 'coordinates':[1,2, 3, 4]}")));
        ASSERT_FALSE(GeoParser::isLine(
            fromjson("{'type':'LineString', 'coordinates':[[1,2, 3], [3,4, 5], [5,6]]}")));
    }

    TEST(GeoParser, isValidPolygon) {
        ASSERT_TRUE(GeoParser::isPolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]] ]}")));
        // No out of bounds points
        ASSERT_FALSE(GeoParser::isPolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,91],[0,5],[0,0]] ]}")));
        ASSERT_TRUE(GeoParser::isPolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[180,0],[5,5],[0,5],[0,0]] ]}")));
        ASSERT_FALSE(GeoParser::isPolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[181,0],[5,5],[0,5],[0,0]] ]}")));
        // And one with a hole.
        ASSERT_TRUE(GeoParser::isPolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]],"
                     " [[1,1],[4,1],[4,4],[1,4],[1,1]] ]}")));
        // Latitudes must be OK
        ASSERT_FALSE(GeoParser::isPolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,91],[0,91],[0,0]],"
                     " [[1,1],[4,1],[4,4],[1,4],[1,1]] ]}")));
        // First point must be the same as the last.
        ASSERT_FALSE(GeoParser::isPolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[1,2],[3,4],[5,6]] ]}")));
    }

    TEST(GeoParser, parsePoint) {
        PointWithCRS point;
        GeoParser::parsePoint(fromjson("{'type':'Point', 'coordinates': [40, 5]}"), &point);
        GeoParser::parsePoint(fromjson("{'type':'Point', 'coordinates': [-4.3, -5.0]}"), &point);
    }

    TEST(GeoParser, parseLine) {
        LineWithCRS polyline;
        GeoParser::parseLine(
            fromjson("{'type':'LineString', 'coordinates':[[1,2],[3,4]]}"),
            &polyline);
        GeoParser::parseLine(
            fromjson("{'type':'LineString', 'coordinates':[[1,2], [3,4], [5,6]]}"),
            &polyline);
        GeoParser::parseLine(
            fromjson("{'type':'LineString', 'coordinates':[[1,2], [3,4], [5,6]]}"),
            &polyline);
    }

    TEST(GeoParser, parsePolygon) {
        PointWithCRS point;
        GeoParser::parsePoint(fromjson("{'type':'Point', 'coordinates': [2, 2]}"),
                                  &point);

        PolygonWithCRS polygonA;
        GeoParser::parsePolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]] ]}"),
            &polygonA);
        ASSERT_TRUE(polygonA.polygon.Contains(point.point));

        PolygonWithCRS polygonB;
        GeoParser::parsePolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]],"
                     " [[1,1],[1,4],[4,4],[4,1],[1,1]] ]}"),
            &polygonB);
        // We removed this in the hole.
        ASSERT_FALSE(polygonB.polygon.Contains(point.point));

        // Now we reverse the orientations and verify that the code fixes it up
        // (outer loop must be CCW, inner CW).
        PolygonWithCRS polygonC;
        GeoParser::parsePolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[0,5],[5,5],[5,0],[0,0]] ]}"),
            &polygonC);
        ASSERT_TRUE(polygonC.polygon.Contains(point.point));

        PolygonWithCRS polygonD;
        GeoParser::parsePolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[0,5],[5,5],[5,0],[0,0]],"
                     " [[1,1],[1,4],[4,4],[4,1],[1,1]] ]}"),
            &polygonD);
        // Also removed in the loop.
        ASSERT_FALSE(polygonD.polygon.Contains(point.point));
    }

    TEST(GeoParser, legacyPoint) {
        PointWithCRS point;
        ASSERT(GeoParser::isPoint(BSON_ARRAY(0 << 1)));
        ASSERT_FALSE(GeoParser::isPoint(BSON_ARRAY(0)));
        ASSERT_FALSE(GeoParser::isPoint(BSON_ARRAY(0 << 1 << 2)));
        ASSERT(GeoParser::isPoint(fromjson("{x: 50, y:40}")));
        ASSERT_FALSE(GeoParser::isPoint(fromjson("{x: '50', y:40}")));
        ASSERT_FALSE(GeoParser::isPoint(fromjson("{x: 5, y:40, z:50}")));
        ASSERT_FALSE(GeoParser::isPoint(fromjson("{x: 5}")));
    }

    TEST(GeoParser, verifyCRS) {
        string goodCRS1 = "crs:{ type: 'name', properties:{name:'EPSG:4326'}}";
        string goodCRS2 = "crs:{ type: 'name', properties:{name:'urn:ogc:def:crs:OGC:1.3:CRS84'}}";
        string badCRS1 = "crs:{ type: 'name', properties:{name:'EPSG:2000'}}";
        string badCRS2 = "crs:{ type: 'name', properties:{name:'urn:ogc:def:crs:OGC:1.3:CRS83'}}";

        BSONObj point1 = fromjson("{'type':'Point', 'coordinates': [40, 5], " + goodCRS1 + "}");
        BSONObj point2 = fromjson("{'type':'Point', 'coordinates': [40, 5], " + goodCRS2 + "}");
        ASSERT(GeoParser::isPoint(point1));
        ASSERT(GeoParser::crsIsOK(point1));
        ASSERT(GeoParser::isPoint(point2));
        ASSERT(GeoParser::crsIsOK(point2));
        BSONObj point3 = fromjson("{'type':'Point', 'coordinates': [40, 5], " + badCRS1 + "}");
        BSONObj point4 = fromjson("{'type':'Point', 'coordinates': [40, 5], " + badCRS2 + "}");
        ASSERT_FALSE(GeoParser::isPoint(point3));
        ASSERT_FALSE(GeoParser::crsIsOK(point3));
        ASSERT_FALSE(GeoParser::isPoint(point4));
        ASSERT_FALSE(GeoParser::crsIsOK(point4));

        BSONObj polygon1 = fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]],"
                                    " [[1,1],[1,4],[4,4],[4,1],[1,1]] ]," + goodCRS1 + "}");
        ASSERT(GeoParser::isPolygon(polygon1));
        ASSERT(GeoParser::crsIsOK(polygon1));
        BSONObj polygon2 = fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]],"
                                    " [[1,1],[1,4],[4,4],[4,1],[1,1]] ]," + badCRS2 + "}");
        ASSERT_FALSE(GeoParser::isPolygon(polygon2));
        ASSERT_FALSE(GeoParser::crsIsOK(polygon2));

        BSONObj line1 = fromjson("{'type':'LineString', 'coordinates':[[1,2], [3,4], [5,6]]," + goodCRS2 + "}");
        ASSERT(GeoParser::isLine(line1));
        ASSERT(GeoParser::crsIsOK(line1));
        BSONObj line2 = fromjson("{'type':'LineString', 'coordinates':[[1,2], [3,4], [5,6]]," + badCRS1 + "}");
        ASSERT_FALSE(GeoParser::isLine(line2));
        ASSERT_FALSE(GeoParser::crsIsOK(line2));
    }

    TEST(GeoParser, legacyPolygon) {
        PolygonWithCRS polygon;
        GeoParser::parsePolygon(fromjson("{$polygon: [[10,20],[10,40],[30,40],[30,20]]}"),
                                       &polygon);
        ASSERT(polygon.crs == FLAT);

        GeoParser::parsePolygon(fromjson("{$polygon: [[10,20], [10,40], [30,40]]}"), &polygon);
        ASSERT(polygon.crs == FLAT);

        ASSERT_FALSE(GeoParser::isPolygon(fromjson("{$polygon: [[10,20],[10,40]]}")));
        ASSERT_FALSE(GeoParser::isPolygon(fromjson("{$polygon: [['10',20],[10,40],[30,40],[30,20]]}")));
        ASSERT_FALSE(GeoParser::isPolygon(fromjson("{$polygon: [[10,20,30],[10,40],[30,40],[30,20]]}")));
        ASSERT(GeoParser::isPolygon(fromjson("{$polygon: {a:{x:40,y:5},b:{x:40,y:6},c:{x:41,y:6},d:{x:41,y:5}}}")));
    }

    TEST(GeoParser, multiPoint) {
        ASSERT(GeoParser::isMultiPoint(
            fromjson("{'type':'MultiPoint','coordinates':[[1,2],[3,4]]}")));
        ASSERT(GeoParser::isMultiPoint(
            fromjson("{'type':'MultiPoint','coordinates':[[3,4]]}")));
        ASSERT(GeoParser::isMultiPoint(
            fromjson("{'type':'MultiPoint','coordinates':[[1,2],[3,4],[5,6],[7,8]]}")));

        ASSERT_FALSE(GeoParser::isMultiPoint(
            fromjson("{'type':'MultiPoint','coordinates':[]}")));
        ASSERT_FALSE(GeoParser::isMultiPoint(
            fromjson("{'type':'MultiPoint','coordinates':[[181,2],[3,4]]}")));
        ASSERT_FALSE(GeoParser::isMultiPoint(
            fromjson("{'type':'MultiPoint','coordinates':[[1,-91],[3,4]]}")));
        ASSERT_FALSE(GeoParser::isMultiPoint(
            fromjson("{'type':'MultiPoint','coordinates':[[181,2],[3,'chicken']]}")));
    }

    TEST(GeoParser, parseMultiPoint) {
        mongo::MultiPointWithCRS mp;
        GeoParser::parseMultiPoint(fromjson("{'type':'MultiPoint','coordinates':[[1,2],[3,4]]}"),
            &mp);
        GeoParser::parseMultiPoint(fromjson("{'type':'MultiPoint','coordinates':[[3,4]]}"),
            &mp);
        GeoParser::parseMultiPoint(
            fromjson("{'type':'MultiPoint','coordinates':[[1,2],[3,4],[5,6],[7,8]]}"), &mp);
    }

    TEST(GeoParser, multiLineString) {
        ASSERT(GeoParser::isMultiLine(
            fromjson("{'type':'MultiLineString','coordinates':[ [[1,1],[2,2],[3,3]],"
                                                               "[[4,5],[6,7]]]}")));
        ASSERT(GeoParser::isMultiLine(
            fromjson("{'type':'MultiLineString','coordinates':[ [[1,1],[2,2]],"
                                                               "[[4,5],[6,7]]]}")));
        ASSERT(GeoParser::isMultiLine(
            fromjson("{'type':'MultiLineString','coordinates':[ [[1,1],[2,2]]]}")));

        ASSERT(GeoParser::isMultiLine(
            fromjson("{'type':'MultiLineString','coordinates':[ [[1,1],[2,2]],"
                                                               "[[2,2],[1,1]]]}")));
        ASSERT_FALSE(GeoParser::isMultiLine(
            fromjson("{'type':'MultiLineString','coordinates':[ [[1,1]]]}")));
        ASSERT_FALSE(GeoParser::isMultiLine(
            fromjson("{'type':'MultiLineString','coordinates':[ [[1,1]],[[1,2],[3,4]]]}")));
        ASSERT_FALSE(GeoParser::isMultiLine(
            fromjson("{'type':'MultiLineString','coordinates':[ [[181,1],[2,2]]]}")));
        ASSERT_FALSE(GeoParser::isMultiLine(
            fromjson("{'type':'MultiLineString','coordinates':[ [[181,1],[2,-91]]]}")));
    }

    TEST(GeoParser, parseMultiLine) {
        mongo::MultiLineWithCRS mls;

        GeoParser::parseMultiLine(
            fromjson("{'type':'MultiLine','coordinates':[ [[1,1],[2,2],[3,3]],"
                                                               "[[4,5],[6,7]]]}"),
            &mls);
                                                               
        GeoParser::parseMultiLine(
            fromjson("{'type':'MultiLine','coordinates':[ [[1,1],[2,2]],"
                                                               "[[4,5],[6,7]]]}"),
            &mls);

        GeoParser::parseMultiLine(
            fromjson("{'type':'MultiLine','coordinates':[ [[1,1],[2,2]]]}"),
            &mls);

        GeoParser::parseMultiLine(
            fromjson("{'type':'MultiLine','coordinates':[ [[1,1],[2,2]],"
                                                               "[[2,2],[1,1]]]}"),
            &mls);
    }

    TEST(GeoParser, multiPolygon) {
        ASSERT(GeoParser::isMultiPolygon(
            fromjson("{'type':'MultiPolygon','coordinates':["
                "[[[102.0, 2.0], [103.0, 2.0], [103.0, 3.0], [102.0, 3.0], [102.0, 2.0]]],"
                "[[[100.0, 0.0], [101.0, 0.0], [101.0, 1.0], [100.0, 1.0], [100.0, 0.0]],"
                 "[[100.2, 0.2], [100.8, 0.2], [100.8, 0.8], [100.2, 0.8], [100.2, 0.2]]]"
                 "]}")));
        ASSERT(GeoParser::isMultiPolygon(
            fromjson("{'type':'MultiPolygon','coordinates':["
                "[[[100.0, 0.0], [101.0, 0.0], [101.0, 1.0], [100.0, 1.0], [100.0, 0.0]],"
                 "[[100.2, 0.2], [100.8, 0.2], [100.8, 0.8], [100.2, 0.8], [100.2, 0.2]]]"
                 "]}")));
    }

    TEST(GeoParser, parseMultiPolygon) {
        mongo::MultiPolygonWithCRS mp;
        GeoParser::parseMultiPolygon(
            fromjson("{'type':'MultiPolygon','coordinates':["
                "[[[102.0, 2.0], [103.0, 2.0], [103.0, 3.0], [102.0, 3.0], [102.0, 2.0]]],"
                "[[[100.0, 0.0], [101.0, 0.0], [101.0, 1.0], [100.0, 1.0], [100.0, 0.0]],"
                 "[[100.2, 0.2], [100.8, 0.2], [100.8, 0.8], [100.2, 0.8], [100.2, 0.2]]]"
                 "]}"), &mp);
    }

    TEST(GeoParser, parseGeometryCollection) {
        {
            mongo::GeometryCollection gc;
            BSONObj obj = fromjson(
            "{ 'type': 'GeometryCollection', 'geometries': ["
             "{ 'type': 'Point','coordinates': [100.0,0.0]},"
             "{ 'type': 'LineString', 'coordinates': [ [101.0, 0.0], [102.0, 1.0] ]}"
             "]}");
            ASSERT(GeoParser::isGeometryCollection(obj));
            GeoParser::parseGeometryCollection(obj, &gc);
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

            ASSERT(GeoParser::isGeometryCollection(obj));
            mongo::GeometryCollection gc;
            GeoParser::parseGeometryCollection(obj, &gc);
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

            ASSERT_FALSE(GeoParser::isGeometryCollection(obj));
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

            ASSERT(GeoParser::isGeometryCollection(obj));
            mongo::GeometryCollection gc;
            GeoParser::parseGeometryCollection(obj, &gc);
            ASSERT_TRUE(gc.supportsContains());
        }
    }
}
