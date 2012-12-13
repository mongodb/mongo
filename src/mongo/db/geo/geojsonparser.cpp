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

#include <string>
#include <vector>
#include "mongo/db/jsobj.h"
#include "mongo/db/geo/geojsonparser.h"
#include "third_party/s2/s2.h"
#include "third_party/s2/s2cell.h"
#include "third_party/s2/s2latlng.h"
#include "third_party/s2/s2loop.h"
#include "third_party/s2/s2polygon.h"
#include "third_party/s2/s2polygonbuilder.h"
#include "third_party/s2/s2polyline.h"

// TODO(hk): Modify S2 library to make DCHECKS work like fassert.
// TODO(hk): Consider moving s/field_parser.cpp into its own library and make use of it instead of
// doing it ourselves.

namespace mongo {
    // This field must be present, and...
    static const string GEOJSON_TYPE = "type";
    // Have one of these three values:
    static const string GEOJSON_TYPE_POINT = "Point";
    static const string GEOJSON_TYPE_LINESTRING = "LineString";
    static const string GEOJSON_TYPE_POLYGON = "Polygon";
    // This field must also be present.  The value depends on the type.
    static const string GEOJSON_COORDINATES = "coordinates";

    //// Utility functions used by GeoJSONParser functions below.
    static S2Point coordToPoint(double p0, double p1) {
        return S2LatLng::FromDegrees(p1, p0).Normalized().ToPoint();
    }

    static S2Point coordsToPoint(const vector<BSONElement>& coordElt) {
        return coordToPoint(coordElt[0].Number(), coordElt[1].Number());
    }

    static void parsePoints(const vector<BSONElement>& coordElt, vector<S2Point>* out) {
        for (size_t i = 0; i < coordElt.size(); ++i) {
            const vector<BSONElement>& pointElt = coordElt[i].Array();
            if (pointElt.empty()) { continue; }
            out->push_back(coordsToPoint(pointElt));
        }
    }

    static bool isArrayOfCoordinates(const vector<BSONElement>& coordinateArray) {
        for (size_t i = 0; i < coordinateArray.size(); ++i) {
            // Each coordinate should be an array
            if (Array != coordinateArray[i].type()) { return false; }
            // ...of two
            const vector<BSONElement> &thisCoord = coordinateArray[i].Array();
            if (2 != thisCoord.size()) { return false; }
            // ...numbers.
            for (size_t j = 0; j < thisCoord.size(); ++j) {
                if (!thisCoord[j].isNumber()) { return false; }
            }
        }
        return true;
    }

    // Coordinates looks like [[0,0],[5,0],[5,5],[0,5],[0,0]]
    static bool isLoopClosed(const vector<BSONElement>& coordinates) {
        double x1, y1, x2, y2;
        x1 = coordinates[0].Array()[0].Number();
        y1 = coordinates[0].Array()[1].Number();
        x2 = coordinates[coordinates.size() - 1].Array()[0].Number();
        y2 = coordinates[coordinates.size() - 1].Array()[1].Number();
        return (fabs(x1 - x2) < 1e-6) && fabs(y1 - y2) < 1e-6;
    }

    //// What we publicly export
    bool GeoJSONParser::isGeoJSONPoint(const BSONObj& obj) {
        BSONElement type = obj.getFieldDotted(GEOJSON_TYPE);
        if (type.eoo() || (String != type.type())) { return false; }
        if (GEOJSON_TYPE_POINT != type.String()) { return false; }

        BSONElement coordElt = obj.getFieldDotted(GEOJSON_COORDINATES);
        if (coordElt.eoo() || (Array != coordElt.type())) { return false; }

        const vector<BSONElement>& coordinates = coordElt.Array();
        if (coordinates.size() != 2) { return false; }
        return coordinates[0].isNumber() && coordinates[1].isNumber();
    }

    void GeoJSONParser::parseGeoJSONPoint(const BSONObj& obj, S2Cell* out) {
        S2Point point = coordsToPoint(obj.getFieldDotted(GEOJSON_COORDINATES).Array());
        *out = S2Cell(point);
    }

    void GeoJSONParser::parseGeoJSONPoint(const BSONObj& obj, S2Point* out) {
        const vector<BSONElement>& coords = obj.getFieldDotted(GEOJSON_COORDINATES).Array();
        *out = coordsToPoint(coords);
    }

    bool GeoJSONParser::isGeoJSONLineString(const BSONObj& obj) {
        BSONElement type = obj.getFieldDotted(GEOJSON_TYPE);
        if (type.eoo() || (String != type.type())) { return false; }
        if (GEOJSON_TYPE_LINESTRING != type.String()) { return false; }

        BSONElement coordElt = obj.getFieldDotted(GEOJSON_COORDINATES);
        if (coordElt.eoo() || (Array != coordElt.type())) { return false; }

        const vector<BSONElement>& coordinateArray = coordElt.Array();
        if (coordinateArray.size() < 2) { return false; }
        return isArrayOfCoordinates(coordinateArray);
    }

    void GeoJSONParser::parseGeoJSONLineString(const BSONObj& obj, S2Polyline* out) {
        vector<S2Point> vertices;
        parsePoints(obj.getFieldDotted(GEOJSON_COORDINATES).Array(), &vertices);
        out->Init(vertices);
    }

    bool GeoJSONParser::isGeoJSONPolygon(const BSONObj& obj) {
        BSONElement type = obj.getFieldDotted(GEOJSON_TYPE);
        if (type.eoo() || (String != type.type())) { return false; }
        if (GEOJSON_TYPE_POLYGON != type.String()) { return false; }

        BSONElement coordElt = obj.getFieldDotted(GEOJSON_COORDINATES);
        if (coordElt.eoo() || (Array != coordElt.type())) { return false; }

        const vector<BSONElement>& coordinates = coordElt.Array();
        // Must be at least one element, the outer shell
        if (coordinates.empty()) { return false; }
        // Verify that the shell is a bunch'a coordinates.
        for (size_t i = 0; i < coordinates.size(); ++i) {
            if (Array != coordinates[i].type()) { return false; }
            const vector<BSONElement>& thisLoop = coordinates[i].Array();
            // A triangle is the simplest 2d shape, and we repeat a vertex, so, 4.
            if (thisLoop.size() < 4) { return false; }
            if (!isArrayOfCoordinates(thisLoop)) { return false; }
            if (!isLoopClosed(thisLoop)) { return false; }
        }
        return true;
    }

    void fixOrientationTo(vector<S2Point>* points, const bool wantClockwise) {
        const vector<S2Point>& pointsRef = *points;
        massert(16463, "Don't have enough points in S2 orientation fixing to work with",
                4 <= points->size());
        double sum = 0;
        // Sum the area under the curve...well really, it's twice the area.
        for (size_t i = 0; i < pointsRef.size(); ++i) {
            // TODO(hk): Convince yourself thoroughly that this is right.
            S2Point a = pointsRef[i];
            S2Point b = pointsRef[(i + 1) % pointsRef.size()];
            sum += (b[1] - a[1]) * (b[0] - a[0]);
        }
        // If sum > 0, clockwise
        // If sum < 0, counter-clockwise
        bool isClockwise = sum > 0;
        if (isClockwise != wantClockwise) {
            vector<S2Point> reversed(pointsRef.rbegin(), pointsRef.rend());
            *points = reversed;
        }
    }

    void GeoJSONParser::parseGeoJSONPolygon(const BSONObj& obj, S2Polygon* out) {
        const vector<BSONElement>& coordinates =
            obj.getFieldDotted(GEOJSON_COORDINATES).Array();

        const vector<BSONElement>& exteriorRing = coordinates[0].Array();
        vector<S2Point> exteriorVertices;
        parsePoints(exteriorRing, &exteriorVertices);

        // The exterior ring must be counter-clockwise.  We fix it up for the user.
        fixOrientationTo(&exteriorVertices, false);

        S2PolygonBuilderOptions polyOptions;
        polyOptions.set_validate(true);
        S2PolygonBuilder polyBuilder(polyOptions);
        S2Loop exteriorLoop(exteriorVertices);
        polyBuilder.AddLoop(&exteriorLoop);

        // Subsequent arrays of coordinates are interior rings/holes.
        for (size_t i = 1; i < coordinates.size(); ++i) {
            vector<S2Point> holePoints;
            parsePoints(coordinates[i].Array(), &holePoints);
            // Interior rings are clockwise.
            fixOrientationTo(&holePoints, true);
            S2Loop holeLoop(holePoints);
            polyBuilder.AddLoop(&holeLoop);
        }

        polyBuilder.AssemblePolygon(out, NULL);
    }

    bool GeoJSONParser::parsePoint(const BSONObj &obj, S2Point *out) {
        if (isGeoJSONPoint(obj)) {
            parseGeoJSONPoint(obj, out);
            return true;
        } else if (isLegacyPoint(obj)) {
            BSONObjIterator it(obj);
            BSONElement x = it.next();
            BSONElement y = it.next();
            *out = coordToPoint(x.number(), y.number());
            return true;
        }
        return false;
    }

    bool GeoJSONParser::parsePoint(const BSONObj &obj, S2Cell *out) {
        S2Point point;
        if (parsePoint(obj, &point)) {
            *out = S2Cell(point);
            return true;
        }
        return false;
    }

    bool GeoJSONParser::parseLineString(const BSONObj &obj, S2Polyline *out) {
        if (!isGeoJSONLineString(obj)) { return false; }
        parseGeoJSONLineString(obj, out);
        return true;
    }

    void GeoJSONParser::parseLegacyPoint(const BSONObj &obj, S2Point *out) {
        BSONObjIterator it(obj);
        BSONElement x = it.next();
        BSONElement y = it.next();
        *out = coordToPoint(x.number(), y.number());
    }

    void GeoJSONParser::parseLegacyPolygon(const BSONObj &obj, S2Polygon *out) {
        vector<S2Point> points;

        BSONObjIterator coordIt(obj);
        while (coordIt.more()) {
            BSONElement coord = coordIt.next();
            S2Point point;
            parseLegacyPoint(coord.Obj(), &point);
            points.push_back(point);
        }
        points.push_back(points[0]);

        fixOrientationTo(&points, false);

        S2PolygonBuilderOptions polyOptions;
        polyOptions.set_validate(true);
        S2PolygonBuilder polyBuilder(polyOptions);
        S2Loop exteriorLoop(points);
        polyBuilder.AddLoop(&exteriorLoop);
        polyBuilder.AssemblePolygon(out, NULL);
    }

    bool GeoJSONParser::parsePolygon(const BSONObj &obj, S2Polygon *out) {
        if (isGeoJSONPolygon(obj)) {
            parseGeoJSONPolygon(obj, out);
            return true;
        } else if (isLegacyPolygon(obj)) {
            parseLegacyPolygon(obj, out);
            return true;
        } else {
            return false;
        }
    }

    bool GeoJSONParser::isLegacyPoint(const BSONObj &obj) {
        BSONObjIterator it(obj);
        if (!it.more()) { return false; }
        BSONElement x = it.next();
        if (!x.isNumber()) { return false; }
        if (!it.more()) { return false; }
        BSONElement y = it.next();
        if (!y.isNumber()) { return false; }
        if (it.more()) { return false; }
        return true;
    }

    bool GeoJSONParser::isLegacyPolygon(const BSONObj &obj) {
        BSONObjIterator coordIt(obj);
        int vertices = 0;
        while (coordIt.more()) {
            BSONElement coord = coordIt.next();
            if (!coord.isABSONObj()) { return false; }
            if (!isLegacyPoint(coord.Obj())) { return false; }
            ++vertices;
        }
        if (vertices < 3) { return false; }
        return true;
    }

    bool GeoJSONParser::isPoint(const BSONObj &obj) {
        return isGeoJSONPoint(obj) || isLegacyPoint(obj);
    }

    bool GeoJSONParser::isLineString(const BSONObj &obj) {
        return isGeoJSONLineString(obj);
    }

    bool GeoJSONParser::isPolygon(const BSONObj &obj) {
        return isGeoJSONPolygon(obj) || isLegacyPolygon(obj);
    }
}  // namespace mongo
