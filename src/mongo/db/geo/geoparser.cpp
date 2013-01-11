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
#include "mongo/db/geo/geoparser.h"
#include "mongo/util/mongoutils/str.h"
#include "third_party/s2/s2.h"
#include "third_party/s2/s2cap.h"
#include "third_party/s2/s2cell.h"
#include "third_party/s2/s2latlng.h"
#include "third_party/s2/s2loop.h"
#include "third_party/s2/s2polygon.h"
#include "third_party/s2/s2polygonbuilder.h"
#include "third_party/s2/s2polyline.h"

namespace mongo {
    // This field must be present, and...
    static const string GEOJSON_TYPE = "type";
    // Have one of these three values:
    static const string GEOJSON_TYPE_POINT = "Point";
    static const string GEOJSON_TYPE_LINESTRING = "LineString";
    static const string GEOJSON_TYPE_POLYGON = "Polygon";
    // This field must also be present.  The value depends on the type.
    static const string GEOJSON_COORDINATES = "coordinates";

    //// Utility functions used by GeoParser functions below.
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
            // ...where the latitude is valid
            double lat = thisCoord[1].Number();
            if (lat < -90 || lat > 90) { return false; }
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
    bool GeoParser::isGeoJSONPoint(const BSONObj& obj) {
        BSONElement type = obj.getFieldDotted(GEOJSON_TYPE);
        if (type.eoo() || (String != type.type())) { return false; }
        if (GEOJSON_TYPE_POINT != type.String()) { return false; }

        if (!crsIsOK(obj)) {
            warning() << "Invalid CRS: " << obj.toString() << endl;
            return false;
        }

        BSONElement coordElt = obj.getFieldDotted(GEOJSON_COORDINATES);
        if (coordElt.eoo() || (Array != coordElt.type())) { return false; }

        const vector<BSONElement>& coordinates = coordElt.Array();
        if (coordinates.size() != 2) { return false; }
        if (!coordinates[0].isNumber() || !coordinates[1].isNumber()) { return false; }
        double lat = coordinates[1].Number();
        return lat >= -90 && lat <= 90;
    }

    void GeoParser::parseGeoJSONPoint(const BSONObj& obj, S2Cell* out) {
        S2Point point = coordsToPoint(obj.getFieldDotted(GEOJSON_COORDINATES).Array());
        *out = S2Cell(point);
    }

    void GeoParser::parseGeoJSONPoint(const BSONObj& obj, S2Point* out) {
        const vector<BSONElement>& coords = obj.getFieldDotted(GEOJSON_COORDINATES).Array();
        *out = coordsToPoint(coords);
    }

    bool GeoParser::isGeoJSONLineString(const BSONObj& obj) {
        BSONElement type = obj.getFieldDotted(GEOJSON_TYPE);
        if (type.eoo() || (String != type.type())) { return false; }
        if (GEOJSON_TYPE_LINESTRING != type.String()) { return false; }

        if (!crsIsOK(obj)) {
            warning() << "Invalid CRS: " << obj.toString() << endl;
            return false;
        }

        BSONElement coordElt = obj.getFieldDotted(GEOJSON_COORDINATES);
        if (coordElt.eoo() || (Array != coordElt.type())) { return false; }

        const vector<BSONElement>& coordinateArray = coordElt.Array();
        if (coordinateArray.size() < 2) { return false; }
        return isArrayOfCoordinates(coordinateArray);
    }

    void GeoParser::parseGeoJSONLineString(const BSONObj& obj, S2Polyline* out) {
        vector<S2Point> vertices;
        parsePoints(obj.getFieldDotted(GEOJSON_COORDINATES).Array(), &vertices);
        out->Init(vertices);
    }

    bool GeoParser::isGeoJSONPolygon(const BSONObj& obj) {
        BSONElement type = obj.getFieldDotted(GEOJSON_TYPE);
        if (type.eoo() || (String != type.type())) { return false; }
        if (GEOJSON_TYPE_POLYGON != type.String()) { return false; }

        if (!crsIsOK(obj)) {
            warning() << "Invalid CRS: " << obj.toString() << endl;
            return false;
        }

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

    void GeoParser::parseGeoJSONPolygon(const BSONObj& obj, S2Polygon* out) {
        const vector<BSONElement>& coordinates =
            obj.getFieldDotted(GEOJSON_COORDINATES).Array();

        const vector<BSONElement>& exteriorRing = coordinates[0].Array();
        vector<S2Point> exteriorVertices;
        parsePoints(exteriorRing, &exteriorVertices);

        S2PolygonBuilderOptions polyOptions;
        polyOptions.set_validate(true);
        S2PolygonBuilder polyBuilder(polyOptions);
        S2Loop exteriorLoop(exteriorVertices);
        if (exteriorLoop.is_hole()) {
            exteriorLoop.Invert();
        }
        polyBuilder.AddLoop(&exteriorLoop);

        // Subsequent arrays of coordinates are interior rings/holes.
        for (size_t i = 1; i < coordinates.size(); ++i) {
            vector<S2Point> holePoints;
            parsePoints(coordinates[i].Array(), &holePoints);
            // Interior rings are clockwise.
            S2Loop holeLoop(holePoints);
            if (!holeLoop.is_hole()) {
                holeLoop.Invert();
            }
            polyBuilder.AddLoop(&holeLoop);
        }

        polyBuilder.AssemblePolygon(out, NULL);
    }

    bool GeoParser::parsePoint(const BSONObj &obj, S2Point *out) {
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

    bool GeoParser::parsePoint(const BSONObj &obj, S2Cell *out) {
        S2Point point;
        if (parsePoint(obj, &point)) {
            *out = S2Cell(point);
            return true;
        }
        return false;
    }

    bool GeoParser::parseLineString(const BSONObj &obj, S2Polyline *out) {
        if (!isGeoJSONLineString(obj)) { return false; }
        parseGeoJSONLineString(obj, out);
        return true;
    }

    void GeoParser::parseLegacyPoint(const BSONObj &obj, S2Point *out) {
        BSONObjIterator it(obj);
        BSONElement x = it.next();
        BSONElement y = it.next();
        *out = coordToPoint(x.number(), y.number());
    }

    bool GeoParser::parsePolygon(const BSONObj &obj, S2Polygon *out) {
        if (isGeoJSONPolygon(obj)) {
            parseGeoJSONPolygon(obj, out);
            return true;
        } else {
            return false;
        }
    }

    bool GeoParser::isLegacyPoint(const BSONObj &obj) {
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

    bool GeoParser::isLegacyPolygon(const BSONObj &obj) {
        BSONObjIterator typeIt(obj);
        BSONElement type = typeIt.next();
        if (!type.isABSONObj()) { return false; }
        if (!mongoutils::str::equals(type.fieldName(), "$polygon")) { return false; }
        BSONObjIterator coordIt(type.embeddedObject());
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

    bool GeoParser::isPoint(const BSONObj &obj) {
        return isGeoJSONPoint(obj) || isLegacyPoint(obj);
    }

    bool GeoParser::isLineString(const BSONObj &obj) {
        return isGeoJSONLineString(obj);
    }

    bool GeoParser::isPolygon(const BSONObj &obj) {
        return isGeoJSONPolygon(obj) || isLegacyPolygon(obj);
    }

    bool GeoParser::crsIsOK(const BSONObj &obj) {
        if (!obj.hasField("crs")) { return true; }

        if (!obj["crs"].isABSONObj()) { return false; }

        BSONObj crsObj = obj["crs"].embeddedObject();
        if (!crsObj.hasField("type")) { return false; }
        if (String != crsObj["type"].type()) { return false; }
        if ("name" != crsObj["type"].String()) { return false; }
        if (!crsObj.hasField("properties")) { return false; }
        if (!crsObj["properties"].isABSONObj()) { return false; }

        BSONObj propertiesObj = crsObj["properties"].embeddedObject();
        if (!propertiesObj.hasField("name")) { return false; }
        if (String != propertiesObj["name"].type()) { return false; }
        const string& name = propertiesObj["name"].String();

        // see http://portal.opengeospatial.org/files/?artifact_id=24045
        // and http://spatialreference.org/ref/epsg/4326/
        // and http://www.geojson.org/geojson-spec.html#named-crs
        return ("urn:ogc:def:crs:OGC:1.3:CRS84" == name) || ("EPSG:4326" == name);
    }

    void GeoParser::parseLegacyPoint(const BSONObj &obj, Point *out) {
        BSONObjIterator it(obj);
        BSONElement x = it.next();
        BSONElement y = it.next();
        out->x = x.number();
        out->y = y.number();
    }

    bool GeoParser::isLegacyBox(const BSONObj &obj) {
        BSONObjIterator typeIt(obj);
        BSONElement type = typeIt.next();
        if (!type.isABSONObj()) { return false; }
        if (!mongoutils::str::equals(type.fieldName(), "$box")) { return false; }
        BSONObjIterator coordIt(type.embeddedObject());
        BSONElement minE = coordIt.next();
        if (!minE.isABSONObj()) { return false; }
        if (!isLegacyPoint(minE.Obj())) { return false; }
        if (!coordIt.more()) { return false; }
        BSONElement maxE = coordIt.next();
        if (!maxE.isABSONObj()) { return false; }
        if (!isLegacyPoint(maxE.Obj())) { return false; }
        return true;
    }

    void GeoParser::parseLegacyBox(const BSONObj &obj, Box *out) {
        BSONObjIterator typeIt(obj);
        BSONElement type = typeIt.next();
        BSONObjIterator coordIt(type.embeddedObject());
        BSONElement minE = coordIt.next();
        BSONElement maxE = coordIt.next();
        parseLegacyPoint(minE.Obj(), &out->_min);
        parseLegacyPoint(maxE.Obj(), &out->_max);
    }

    bool GeoParser::isLegacyCenter(const BSONObj &obj) {
        BSONObjIterator typeIt(obj);
        BSONElement type = typeIt.next();
        if (!type.isABSONObj()) { return false; }
        bool isCenter = mongoutils::str::equals(type.fieldName(), "$center");
        if (!isCenter) { return false; }
        BSONObjIterator objIt(type.embeddedObject());
        BSONElement center = objIt.next();
        if (!center.isABSONObj()) { return false; }
        if (!isLegacyPoint(center.Obj())) { return false; }
        if (!objIt.more()) { return false; }
        BSONElement radius = objIt.next();
        if (!radius.isNumber()) { return false; }
        return true;
    }

    void GeoParser::parseLegacyCenter(const BSONObj &obj, Circle *out) {
        BSONObjIterator typeIt(obj);
        BSONElement type = typeIt.next();
        BSONObjIterator objIt(type.embeddedObject());
        BSONElement center = objIt.next();
        parseLegacyPoint(center.Obj(), &out->center);
        BSONElement radius = objIt.next();
        out->radius = radius.number();
    }

    bool GeoParser::isLegacyCenterSphere(const BSONObj &obj) {
        BSONObjIterator typeIt(obj);
        BSONElement type = typeIt.next();
        if (!type.isABSONObj()) { return false; }
        bool isCenterSphere = mongoutils::str::equals(type.fieldName(), "$centerSphere");
        if (!isCenterSphere) { return false; }
        BSONObjIterator objIt(type.embeddedObject());
        BSONElement center = objIt.next();
        if (!center.isABSONObj()) { return false; }
        if (!isLegacyPoint(center.Obj())) { return false; }
        if (!objIt.more()) { return false; }
        BSONElement radius = objIt.next();
        if (!radius.isNumber()) { return false; }
        return true;
    }

    void GeoParser::parseLegacyCenterSphere(const BSONObj &obj, S2Cap *out) {
        BSONObjIterator typeIt(obj);
        BSONElement type = typeIt.next();
        BSONObjIterator objIt(type.embeddedObject());
        BSONElement center = objIt.next();
        S2Point centerPoint;
        parseLegacyPoint(center.Obj(), &centerPoint);
        BSONElement radiusElt = objIt.next();
        double radius = radiusElt.number();
        *out = S2Cap::FromAxisAngle(centerPoint, S1Angle::Radians(radius));
    }

    void GeoParser::parseLegacyPolygon(const BSONObj &obj, Polygon *out) {
        BSONObjIterator typeIt(obj);
        BSONElement type = typeIt.next();
        BSONObjIterator coordIt(type.embeddedObject());
        vector<Point> points;
        while (coordIt.more()) {
            Point p;
            parseLegacyPoint(coordIt.next().Obj(), &p);
            points.push_back(p);
        }
        *out = Polygon(points);
    }

    bool GeoParser::parsePolygon(const BSONObj &obj, Polygon *out) {
        if (!isLegacyPolygon(obj)) { return false; }
        parseLegacyPolygon(obj, out);
        return true;
    }
}  // namespace mongo
