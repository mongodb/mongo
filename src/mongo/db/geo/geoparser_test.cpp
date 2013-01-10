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
 */

/**
 * This file contains tests for mongo/db/geo/geoparser.cpp.
 */

#include <string>
#include <sstream>

#include "mongo/db/geo/geoparser.h"
#include "mongo/db/json.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include "third_party/s2/s2.h"
#include "third_party/s2/s2polygon.h"
#include "third_party/s2/s2polyline.h"

using mongo::BSONObj;
using mongo::fromjson;
using mongo::GeoParser;

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
        ASSERT_FALSE(GeoParser::isPoint(fromjson("{'type':'Point', 'coordinates': [0, 90.1]}")));
        ASSERT_FALSE(GeoParser::isPoint(fromjson("{'type':'Point', 'coordinates': [0, -90.1]}")));
    }

    TEST(GeoParser, isValidLineString) {
        ASSERT_TRUE(GeoParser::isLineString(
            fromjson("{'type':'LineString', 'coordinates':[[1,2], [3,4]]}")));
        ASSERT_TRUE(GeoParser::isLineString(
            fromjson("{'type':'LineString', 'coordinates':[[0,-90], [0,90]]}")));
        ASSERT_FALSE(GeoParser::isLineString(
            fromjson("{'type':'LineString', 'coordinates':[[0,-91], [0,90]]}")));
        ASSERT_FALSE(GeoParser::isLineString(
            fromjson("{'type':'LineString', 'coordinates':[[0,-90], [0,91]]}")));
        ASSERT_TRUE(GeoParser::isLineString(
            fromjson("{'type':'LineString', 'coordinates':[[1,2], [3,4], [5,6]]}")));
        ASSERT_FALSE(GeoParser::isLineString(
            fromjson("{'type':'LineString', 'coordinates':[[1,2]]}")));
        ASSERT_FALSE(GeoParser::isLineString(
            fromjson("{'type':'LineString', 'coordinates':[['chicken','little']]}")));
        ASSERT_FALSE(GeoParser::isLineString(
            fromjson("{'type':'LineString', 'coordinates':[1,2, 3, 4]}")));
        ASSERT_FALSE(GeoParser::isLineString(
            fromjson("{'type':'LineString', 'coordinates':[[1,2, 3], [3,4, 5], [5,6]]}")));
    }

    TEST(GeoParser, isValidPolygon) {
        ASSERT_TRUE(GeoParser::isPolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]] ]}")));
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
        S2Point point;
        GeoParser::parsePoint(fromjson("{'type':'Point', 'coordinates': [40, 5]}"),
                                  &point);
        GeoParser::parsePoint(fromjson("{'type':'Point', 'coordinates': [-40.3, -5.0]}"),
                                  &point);
    }

    TEST(GeoParser, parseLineString) {
        S2Polyline polyline;
        GeoParser::parseLineString(
            fromjson("{'type':'LineString', 'coordinates':[[1,2],[3,4]]}"),
            &polyline);
        GeoParser::parseLineString(
            fromjson("{'type':'LineString', 'coordinates':[[1,2], [3,4], [5,6]]}"),
            &polyline);
        GeoParser::parseLineString(
            fromjson("{'type':'LineString', 'coordinates':[[1,2], [3,4], [5,6]]}"),
            &polyline);
    }

    TEST(GeoParser, parsePolygon) {
        S2Point point;
        GeoParser::parsePoint(fromjson("{'type':'Point', 'coordinates': [2, 2]}"),
                                  &point);

        S2Polygon polygonA;
        GeoParser::parsePolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]] ]}"),
            &polygonA);
        ASSERT_TRUE(polygonA.Contains(point));

        S2Polygon polygonB;
        GeoParser::parsePolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]],"
                     " [[1,1],[1,4],[4,4],[4,1],[1,1]] ]}"),
            &polygonB);
        // We removed this in the hole.
        ASSERT_FALSE(polygonB.Contains(point));

        // Now we reverse the orientations and verify that the code fixes it up
        // (outer loop must be CCW, inner CW).
        S2Polygon polygonC;
        GeoParser::parsePolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[0,5],[5,5],[5,0],[0,0]] ]}"),
            &polygonC);
        ASSERT_TRUE(polygonC.Contains(point));

        S2Polygon polygonD;
        GeoParser::parsePolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[0,5],[5,5],[5,0],[0,0]],"
                     " [[1,1],[1,4],[4,4],[4,1],[1,1]] ]}"),
            &polygonD);
        // Also removed in the loop.
        ASSERT_FALSE(polygonD.Contains(point));
    }

    TEST(GeoParser, parseLegacyPoint) {
        S2Point point;
        ASSERT(GeoParser::parsePoint(BSON_ARRAY(0 << 1), &point));
        ASSERT_FALSE(GeoParser::parsePoint(BSON_ARRAY(0), &point));
        ASSERT_FALSE(GeoParser::parsePoint(BSON_ARRAY(0 << 1 << 2), &point));
        ASSERT(GeoParser::parsePoint(fromjson("{x: 50, y:40}"), &point));
        ASSERT_FALSE(GeoParser::parsePoint(fromjson("{x: '50', y:40}"), &point));
        ASSERT_FALSE(GeoParser::parsePoint(fromjson("{x: 5, y:40, z:50}"), &point));
        ASSERT_FALSE(GeoParser::parsePoint(fromjson("{x: 5}"), &point));
    }

    TEST(GeoParser, verifyCRS) {
        string goodCRS1 = "crs:{ type: 'name', properties:{name:'EPSG:4326'}}";
        string goodCRS2 = "crs:{ type: 'name', properties:{name:'urn:ogc:def:crs:OGC:1.3:CRS84'}}";
        string badCRS1 = "crs:{ type: 'name', properties:{name:'EPSG:2000'}}";
        string badCRS2 = "crs:{ type: 'name', properties:{name:'urn:ogc:def:crs:OGC:1.3:CRS83'}}";

        BSONObj point1 = fromjson("{'type':'Point', 'coordinates': [40, 5], " + goodCRS1 + "}");
        BSONObj point2 = fromjson("{'type':'Point', 'coordinates': [40, 5], " + goodCRS2 + "}");
        ASSERT(GeoParser::isGeoJSONPoint(point1));
        ASSERT(GeoParser::crsIsOK(point1));
        ASSERT(GeoParser::isGeoJSONPoint(point2));
        ASSERT(GeoParser::crsIsOK(point2));
        BSONObj point3 = fromjson("{'type':'Point', 'coordinates': [40, 5], " + badCRS1 + "}");
        BSONObj point4 = fromjson("{'type':'Point', 'coordinates': [40, 5], " + badCRS2 + "}");
        ASSERT_FALSE(GeoParser::isGeoJSONPoint(point3));
        ASSERT_FALSE(GeoParser::crsIsOK(point3));
        ASSERT_FALSE(GeoParser::isGeoJSONPoint(point4));
        ASSERT_FALSE(GeoParser::crsIsOK(point4));

        BSONObj polygon1 = fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]],"
                                    " [[1,1],[1,4],[4,4],[4,1],[1,1]] ]," + goodCRS1 + "}");
        ASSERT(GeoParser::isGeoJSONPolygon(polygon1));
        ASSERT(GeoParser::crsIsOK(polygon1));
        BSONObj polygon2 = fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]],"
                                    " [[1,1],[1,4],[4,4],[4,1],[1,1]] ]," + badCRS2 + "}");
        ASSERT_FALSE(GeoParser::isGeoJSONPolygon(polygon2));
        ASSERT_FALSE(GeoParser::crsIsOK(polygon2));

        BSONObj line1 = fromjson("{'type':'LineString', 'coordinates':[[1,2], [3,4], [5,6]]," + goodCRS2 + "}");
        ASSERT(GeoParser::isGeoJSONLineString(line1));
        ASSERT(GeoParser::crsIsOK(line1));
        BSONObj line2 = fromjson("{'type':'LineString', 'coordinates':[[1,2], [3,4], [5,6]]," + badCRS1 + "}");
        ASSERT_FALSE(GeoParser::isGeoJSONLineString(line2));
        ASSERT_FALSE(GeoParser::crsIsOK(line2));
    }

    TEST(GeoParser, parseLegacyPolygon) {
        S2Polygon polygon;
        ASSERT(GeoParser::parsePolygon(BSON_ARRAY(BSON_ARRAY(10 << 20) << BSON_ARRAY(10 << 40)
                                                      << BSON_ARRAY(30 << 40)
                                                      << BSON_ARRAY(30 << 20)),
                                           &polygon));
        polygon.Release(NULL);
        ASSERT(GeoParser::parsePolygon(BSON_ARRAY(BSON_ARRAY(10 << 20) << BSON_ARRAY(10 << 40)
                                                      << BSON_ARRAY(30 << 40)),
                                           &polygon));
        polygon.Release(NULL);
        ASSERT_FALSE(GeoParser::parsePolygon(BSON_ARRAY(BSON_ARRAY(10 << 20)
                                                            << BSON_ARRAY(10 << 40)),
                                           &polygon));
        polygon.Release(NULL);
        ASSERT_FALSE(GeoParser::parsePolygon(BSON_ARRAY(BSON_ARRAY("10" << 20)
                                                            << BSON_ARRAY(10 << 40)
                                                            << BSON_ARRAY(30 << 40)
                                                            << BSON_ARRAY(30 << 20)),
                                           &polygon));
        polygon.Release(NULL);
        ASSERT_FALSE(GeoParser::parsePolygon(BSON_ARRAY(BSON_ARRAY(10 << 20 << 30)
                                                            << BSON_ARRAY(10 << 40)
                                                            << BSON_ARRAY(30 << 40)
                                                            << BSON_ARRAY(30 << 20)),
                                           &polygon));
        polygon.Release(NULL);
        ASSERT(GeoParser::parsePolygon(
            fromjson("{a:{x:40,y:5},b:{x:40,y:6},c:{x:41,y:6},d:{x:41,y:5}}"), &polygon));
        polygon.Release(NULL);
    }
}
