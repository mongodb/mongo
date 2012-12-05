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
 * This file contains tests for mongo/db/geo/geojson_parser.cpp.
 */

#include <string>
#include <sstream>

#include "mongo/db/geo/geojsonparser.h"
#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include "third_party/s2/s2.h"
#include "third_party/s2/s2polygon.h"
#include "third_party/s2/s2polyline.h"

using mongo::GeoJSONParser;
using mongo::fromjson;

namespace {
    TEST(GeoJSONParser, isValidPoint) {
        ASSERT_TRUE(GeoJSONParser::isPoint(fromjson("{'type':'Point', 'coordinates': [40, 5]}")));
        ASSERT_TRUE(GeoJSONParser::isPoint(
            fromjson("{'type':'Point', 'coordinates': [-40.3, -5.0]}")));
        ASSERT_FALSE(GeoJSONParser::isPoint(fromjson("{'typo':'Point', 'coordinates': [40, -5]}")));
        ASSERT_FALSE(GeoJSONParser::isPoint(fromjson("{'type':'Point', 'coordhats': [40, -5]}")));
        ASSERT_FALSE(GeoJSONParser::isPoint(
            fromjson("{'type':['Point'], 'coordinates': [40, -5]}")));
        ASSERT_FALSE(GeoJSONParser::isPoint(fromjson("{'type':'Point', 'coordinates': 40}")));
        ASSERT_FALSE(GeoJSONParser::isPoint(
            fromjson("{'type':'Point', 'coordinates': [40, -5, 7]}")));
    }

    TEST(GeoJSONParser, isValidLineString) {
        ASSERT_TRUE(GeoJSONParser::isLineString(
            fromjson("{'type':'LineString', 'coordinates':[[1,2], [3,4]]}")));
        ASSERT_TRUE(GeoJSONParser::isLineString(
            fromjson("{'type':'LineString', 'coordinates':[[1,2], [3,4], [5,6]]}")));
        ASSERT_FALSE(GeoJSONParser::isLineString(
            fromjson("{'type':'LineString', 'coordinates':[[1,2]]}")));
        ASSERT_FALSE(GeoJSONParser::isLineString(
            fromjson("{'type':'LineString', 'coordinates':[['chicken','little']]}")));
        ASSERT_FALSE(GeoJSONParser::isLineString(
            fromjson("{'type':'LineString', 'coordinates':[1,2, 3, 4]}")));
        ASSERT_FALSE(GeoJSONParser::isLineString(
            fromjson("{'type':'LineString', 'coordinates':[[1,2, 3], [3,4, 5], [5,6]]}")));
    }

    TEST(GeoJSONParser, isValidPolygon) {
        ASSERT_TRUE(GeoJSONParser::isPolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]] ]}")));
        // And one with a hole.
        ASSERT_TRUE(GeoJSONParser::isPolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]],"
                     " [[1,1],[4,1],[4,4],[1,4],[1,1]] ]}")));
        // First point must be the same as the last.
        ASSERT_FALSE(GeoJSONParser::isPolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[1,2],[3,4],[5,6]] ]}")));
    }

    TEST(GeoJSONParser, parsePoint) {
        GeoJSONParser::Params params;
        params.projection = GeoJSONParser::SPHERE;

        S2Point point;
        GeoJSONParser::parsePoint(fromjson("{'type':'Point', 'coordinates': [40, 5]}"),
                                  params, &point);
        GeoJSONParser::parsePoint(fromjson("{'type':'Point', 'coordinates': [-40.3, -5.0]}"),
                                  params, &point);
    }

    TEST(GeoJSONParser, parseLineString) {
        GeoJSONParser::Params params;
        params.projection = GeoJSONParser::SPHERE;

        S2Polyline polyline;
        GeoJSONParser::parseLineString(
            fromjson("{'type':'LineString', 'coordinates':[[1,2],[3,4]]}"),
            params, &polyline);
        GeoJSONParser::parseLineString(
            fromjson("{'type':'LineString', 'coordinates':[[1,2], [3,4], [5,6]]}"),
            params, &polyline);
    }

    TEST(GeoJSONParser, parsePolygon) {
        GeoJSONParser::Params params;
        params.projection = GeoJSONParser::SPHERE;

        S2Point point;
        GeoJSONParser::parsePoint(fromjson("{'type':'Point', 'coordinates': [2, 2]}"),
                                  params, &point);

        S2Polygon polygonA;
        GeoJSONParser::parsePolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]] ]}"),
            params, &polygonA);
        ASSERT_TRUE(polygonA.Contains(point));

        S2Polygon polygonB;
        GeoJSONParser::parsePolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[5,0],[5,5],[0,5],[0,0]],"
                     " [[1,1],[1,4],[4,4],[4,1],[1,1]] ]}"),
            params, &polygonB);
        // We removed this in the hole.
        ASSERT_FALSE(polygonB.Contains(point));

        // Now we reverse the orientations and verify that the code fixes it up
        // (outer loop must be CCW, inner CW).
        S2Polygon polygonC;
        GeoJSONParser::parsePolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[0,5],[5,5],[5,0],[0,0]] ]}"),
            params, &polygonC);
        ASSERT_TRUE(polygonC.Contains(point));

        S2Polygon polygonD;
        GeoJSONParser::parsePolygon(
            fromjson("{'type':'Polygon', 'coordinates':[ [[0,0],[0,5],[5,5],[5,0],[0,0]],"
                     " [[1,1],[1,4],[4,4],[4,1],[1,1]] ]}"),
            params, &polygonD);
        // Also removed in the loop.
        ASSERT_FALSE(polygonD.Contains(point));
    }

    void fillPlaneParams(GeoJSONParser::Params *out) {
        out->projection = GeoJSONParser::PLANE;
        out->rescaleMaxX = out->rescaleMaxY = 0.5;
        out->rescaleMinX = out->rescaleMinY = -0.5;
        out->maxX = out->maxY = 180;
        out->minX = out->minY = -180;
    }

    TEST(GeoJSONParser, parseLegacyPoint) {
        GeoJSONParser::Params params;
        fillPlaneParams(&params);
        S2Point point;
        ASSERT(GeoJSONParser::parsePoint(BSON_ARRAY(0 << 1), params, &point));
        ASSERT_FALSE(GeoJSONParser::parsePoint(BSON_ARRAY(0), params, &point));
        ASSERT_FALSE(GeoJSONParser::parsePoint(BSON_ARRAY(0 << 1 << 2), params, &point));
        ASSERT(GeoJSONParser::parsePoint(fromjson("{x: 50, y:40}"), params, &point));
        ASSERT_FALSE(GeoJSONParser::parsePoint(fromjson("{x: '50', y:40}"), params, &point));
        ASSERT_FALSE(GeoJSONParser::parsePoint(fromjson("{x: 5, y:40, z:50}"), params, &point));
        ASSERT_FALSE(GeoJSONParser::parsePoint(fromjson("{x: 5}"), params, &point));
    }

    TEST(GeoJSONParser, unprojectedParsePoint) {
        GeoJSONParser::Params params;
        params.projection = GeoJSONParser::LITERAL;
        S2Point point;
        ASSERT(GeoJSONParser::parsePoint(fromjson("{x: 50, y:40}"), params, &point));
        ASSERT_EQUALS(point[0], 50);
        ASSERT_EQUALS(point[1], 40);
    }

    TEST(GeoJSONParser, parseLegacyPolygon) {
        GeoJSONParser::Params params;
        fillPlaneParams(&params);
        S2Polygon polygon;
        ASSERT(GeoJSONParser::parsePolygon(BSON_ARRAY(BSON_ARRAY(10 << 20) << BSON_ARRAY(10 << 40)
                                                      << BSON_ARRAY(30 << 40)
                                                      << BSON_ARRAY(30 << 20)),
                                           params, &polygon));
        polygon.Release(NULL);
        ASSERT(GeoJSONParser::parsePolygon(BSON_ARRAY(BSON_ARRAY(10 << 20) << BSON_ARRAY(10 << 40)
                                                      << BSON_ARRAY(30 << 40)),
                                           params, &polygon));
        polygon.Release(NULL);
        ASSERT_FALSE(GeoJSONParser::parsePolygon(BSON_ARRAY(BSON_ARRAY(10 << 20)
                                                            << BSON_ARRAY(10 << 40)),
                                           params, &polygon));
        polygon.Release(NULL);
        ASSERT_FALSE(GeoJSONParser::parsePolygon(BSON_ARRAY(BSON_ARRAY("10" << 20)
                                                            << BSON_ARRAY(10 << 40)
                                                            << BSON_ARRAY(30 << 40)
                                                            << BSON_ARRAY(30 << 20)),
                                           params, &polygon));
        polygon.Release(NULL);
        ASSERT_FALSE(GeoJSONParser::parsePolygon(BSON_ARRAY(BSON_ARRAY(10 << 20 << 30)
                                                            << BSON_ARRAY(10 << 40)
                                                            << BSON_ARRAY(30 << 40)
                                                            << BSON_ARRAY(30 << 20)),
                                           params, &polygon));
        polygon.Release(NULL);
        ASSERT(GeoJSONParser::parsePolygon(
            fromjson("{a:{x:40,y:5},b:{x:40,y:6},c:{x:41,y:6},d:{x:41,y:5}}"), params, &polygon));
        polygon.Release(NULL);
    }
}
