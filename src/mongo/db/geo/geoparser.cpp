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

#include "mongo/db/geo/geoparser.h"

#include <string>
#include <vector>

#include "mongo/db/geo/shapes.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/mongoutils/str.h"
#include "third_party/s2/s2polygonbuilder.h"

namespace mongo {

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

    static bool isGeoJSONPoint(const BSONObj& obj) {
        BSONElement type = obj.getFieldDotted(GEOJSON_TYPE);
        if (type.eoo() || (String != type.type())) { return false; }
        if (GEOJSON_TYPE_POINT != type.String()) { return false; }

        if (!GeoParser::crsIsOK(obj)) {
            warning() << "Invalid CRS: " << obj.toString() << endl;
            return false;
        }

        BSONElement coordElt = obj.getFieldDotted(GEOJSON_COORDINATES);
        if (coordElt.eoo() || (Array != coordElt.type())) { return false; }

        const vector<BSONElement>& coordinates = coordElt.Array();
        if (coordinates.size() != 2) { return false; }
        if (!coordinates[0].isNumber() || !coordinates[1].isNumber()) { return false; }
        double lat = coordinates[1].Number();
        double lng = coordinates[0].Number();
        return lat >= -90 && lat <= 90 && lng >= -180 && lng <= 180;
    }

    static bool isLegacyPoint(const BSONObj &obj) {
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

    static S2Point coordToPoint(double lng, double lat) {
        // Note that it's (lat, lng) for S2 but (lng, lat) for MongoDB.
        S2LatLng ll = S2LatLng::FromDegrees(lat, lng).Normalized();
        if (!ll.is_valid()) {
            stringstream ss;
            ss << "coords invalid after normalization, lng = " << lng << " lat = " << lat << endl;
            uasserted(17125, ss.str());
        }
        return ll.ToPoint();
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
            double lng = thisCoord[0].Number();
            if (lat < -90 || lat > 90) { return false; }
            if (lng < -180 || lng > 180) { return false; }
        }
        return true;
    }

    static bool parsePoints(const vector<BSONElement>& coordElt, vector<S2Point>* out) {
        for (size_t i = 0; i < coordElt.size(); ++i) {
            const vector<BSONElement>& pointElt = coordElt[i].Array();
            if (pointElt.empty()) { continue; }
            out->push_back(coordToPoint(pointElt[0].Number(), pointElt[1].Number()));
        }

        return true;
    }

    static bool isValidLineString(const vector<BSONElement>& coordinateArray) {
        if (coordinateArray.size() < 2) { return false; }
        if (!isArrayOfCoordinates(coordinateArray)) { return false; }
        vector<S2Point> vertices;
        if (!parsePoints(coordinateArray, &vertices)) { return false; }
        eraseDuplicatePoints(&vertices);
        return S2Polyline::IsValid(vertices);
    }

    static bool parseGeoJSONPolygonCoordinates(const vector<BSONElement>& coordinates,
                                               const BSONObj &sourceObject, S2Polygon *out) {
        const vector<BSONElement>& exteriorRing = coordinates[0].Array();
        vector<S2Point> exteriorVertices;
        if (!parsePoints(exteriorRing, &exteriorVertices)) { return false; }
        // The last point is duplicated.  We drop it, since S2Loop expects no
        // duplicate points
        exteriorVertices.resize(exteriorVertices.size() - 1);

        S2PolygonBuilderOptions polyOptions;
        polyOptions.set_validate(true);
        // Don't silently eliminate duplicate edges.
        polyOptions.set_xor_edges(false);
        S2PolygonBuilder polyBuilder(polyOptions);
        S2Loop exteriorLoop(exteriorVertices);
        exteriorLoop.Normalize();
        if (exteriorLoop.is_hole()) {
            exteriorLoop.Invert();
        }
        if (!exteriorLoop.IsValid()) { return false; }
        polyBuilder.AddLoop(&exteriorLoop);

        // Subsequent arrays of coordinates are interior rings/holes.
        for (size_t i = 1; i < coordinates.size(); ++i) {
            vector<S2Point> holePoints;
            if (!parsePoints(coordinates[i].Array(), &holePoints)) { return false; }
            // Drop the duplicated last point.
            holePoints.resize(holePoints.size() - 1);
            // Interior rings are clockwise.
            S2Loop holeLoop(holePoints);
            holeLoop.Normalize();
            if (!holeLoop.IsValid()) { return false; }
            if (!holeLoop.is_hole()) {
                holeLoop.Invert();
            }
            polyBuilder.AddLoop(&holeLoop);
        }

        return polyBuilder.AssemblePolygon(out, NULL);
    }

    static bool parseLegacyPoint(const BSONObj &obj, Point *out) {
        BSONObjIterator it(obj);
        BSONElement x = it.next();
        BSONElement y = it.next();
        out->x = x.number();
        out->y = y.number();
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

    static bool isGeoJSONPolygonCoordinates(const vector<BSONElement>& coordinates) {
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

    static bool isGeoJSONPolygon(const BSONObj& obj) {
        BSONElement type = obj.getFieldDotted(GEOJSON_TYPE);
        if (type.eoo() || (String != type.type())) { return false; }
        if (GEOJSON_TYPE_POLYGON != type.String()) { return false; }

        if (!GeoParser::crsIsOK(obj)) {
            warning() << "Invalid CRS: " << obj.toString() << endl;
            return false;
        }

        BSONElement coordElt = obj.getFieldDotted(GEOJSON_COORDINATES);
        if (coordElt.eoo() || (Array != coordElt.type())) { return false; }

        return isGeoJSONPolygonCoordinates(coordElt.Array());
    }

    static bool isLegacyPolygon(const BSONObj &obj) {
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

    static bool isLegacyCenter(const BSONObj &obj) {
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

    static bool isLegacyCenterSphere(const BSONObj &obj) {
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

    /** exported **/

    bool GeoParser::isPoint(const BSONObj &obj) {
        return isGeoJSONPoint(obj) || isLegacyPoint(obj);
    }

    bool GeoParser::parsePoint(const BSONObj &obj, PointWithCRS *out) {
        if (isGeoJSONPoint(obj)) {
            const vector<BSONElement>& coords = obj.getFieldDotted(GEOJSON_COORDINATES).Array();
            out->point = coordToPoint(coords[0].Number(), coords[1].Number());
            out->cell = S2Cell(out->point);
            out->oldPoint.x = coords[0].Number();
            out->oldPoint.y = coords[1].Number(); 
            out->crs = SPHERE;
        } else if (isLegacyPoint(obj)) {
            BSONObjIterator it(obj);
            BSONElement x = it.next();
            BSONElement y = it.next();
            out->point = coordToPoint(x.Number(), y.Number());
            out->cell = S2Cell(out->point);
            out->oldPoint.x = x.Number();
            out->oldPoint.y = y.Number(); 
            out->crs = FLAT;
        }

        return true;
    }

    bool GeoParser::isLine(const BSONObj& obj) {
        BSONElement type = obj.getFieldDotted(GEOJSON_TYPE);
        if (type.eoo() || (String != type.type())) { return false; }
        if (GEOJSON_TYPE_LINESTRING != type.String()) { return false; }

        if (!crsIsOK(obj)) {
            warning() << "Invalid CRS: " << obj.toString() << endl;
            return false;
        }

        BSONElement coordElt = obj.getFieldDotted(GEOJSON_COORDINATES);
        if (coordElt.eoo() || (Array != coordElt.type())) { return false; }

        return isValidLineString(coordElt.Array());
    }

    bool GeoParser::parseLine(const BSONObj& obj, LineWithCRS* out) {
        vector<S2Point> vertices;
        if (!parsePoints(obj.getFieldDotted(GEOJSON_COORDINATES).Array(), &vertices)) {
            return false;
        }
        eraseDuplicatePoints(&vertices);
        out->line.Init(vertices);
        out->crs = SPHERE;
        return true;
    }

    bool GeoParser::isBox(const BSONObj &obj) {
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

    bool GeoParser::parseBox(const BSONObj &obj, BoxWithCRS *out) {
        BSONObjIterator typeIt(obj);
        BSONElement type = typeIt.next();
        BSONObjIterator coordIt(type.embeddedObject());
        BSONElement minE = coordIt.next();
        BSONElement maxE = coordIt.next();
        if (!parseLegacyPoint(minE.Obj(), &out->box._min) ||
            !parseLegacyPoint(maxE.Obj(), &out->box._max)) { return false; }
        out->crs = FLAT;
        return true;
    }

    bool GeoParser::parsePolygon(const BSONObj &obj, PolygonWithCRS *out) {
        if (isGeoJSONPolygon(obj)) {
            const vector<BSONElement>& coordinates = obj.getFieldDotted(GEOJSON_COORDINATES).Array();
            if (!parseGeoJSONPolygonCoordinates(coordinates, obj, &out->polygon)) { return false; }
            out->crs = SPHERE;
        } else {
            BSONObjIterator typeIt(obj);
            BSONElement type = typeIt.next();
            BSONObjIterator coordIt(type.embeddedObject());
            vector<Point> points;
            while (coordIt.more()) {
                Point p;
                if (!parseLegacyPoint(coordIt.next().Obj(), &p)) { return false; }
                points.push_back(p);
            }
            out->oldPolygon = Polygon(points);
            out->crs = FLAT;
        }
        return true;
    }

    bool GeoParser::isMultiPoint(const BSONObj &obj) {
        BSONElement type = obj.getFieldDotted(GEOJSON_TYPE);
        if (type.eoo() || (String != type.type())) { return false; }
        if (GEOJSON_TYPE_MULTI_POINT != type.String()) { return false; }

        if (!crsIsOK(obj)) {
            warning() << "Invalid CRS: " << obj.toString() << endl;
            return false;
        }

        BSONElement coordElt = obj.getFieldDotted(GEOJSON_COORDINATES);
        if (coordElt.eoo() || (Array != coordElt.type())) { return false; }

        const vector<BSONElement>& coordinates = coordElt.Array();
        if (0 == coordinates.size()) { return false; }
        return isArrayOfCoordinates(coordinates);
    }

    bool GeoParser::parseMultiPoint(const BSONObj &obj, MultiPointWithCRS *out) {
        out->points.clear();
        BSONElement coordElt = obj.getFieldDotted(GEOJSON_COORDINATES);
        const vector<BSONElement>& coordinates = coordElt.Array();
        out->points.resize(coordinates.size());
        out->cells.resize(coordinates.size());
        for (size_t i = 0; i < coordinates.size(); ++i) {
            const vector<BSONElement>& thisCoord = coordinates[i].Array();
            out->points[i] = coordToPoint(thisCoord[0].Number(), thisCoord[1].Number());
            out->cells[i] = S2Cell(out->points[i]);
        }

        return true;
    }

    bool GeoParser::isMultiLine(const BSONObj &obj) {
        BSONElement type = obj.getFieldDotted(GEOJSON_TYPE);
        if (type.eoo() || (String != type.type())) { return false; }
        if (GEOJSON_TYPE_MULTI_LINESTRING != type.String()) { return false; }

        if (!crsIsOK(obj)) {
            warning() << "Invalid CRS: " << obj.toString() << endl;
            return false;
        }

        BSONElement coordElt = obj.getFieldDotted(GEOJSON_COORDINATES);
        if (coordElt.eoo() || (Array != coordElt.type())) { return false; }

        const vector<BSONElement>& coordinates = coordElt.Array();
        if (0 == coordinates.size()) { return false; }

        for (size_t i = 0; i < coordinates.size(); ++i) {
            if (coordinates[i].eoo() || (Array != coordinates[i].type())) { return false; }
            if (!isValidLineString(coordinates[i].Array())) { return false; }
        }

        return true;
    }

    bool GeoParser::parseMultiLine(const BSONObj &obj, MultiLineWithCRS *out) {
        vector<BSONElement> coordElt = obj.getFieldDotted(GEOJSON_COORDINATES).Array();
        out->lines.mutableVector().clear();
        out->lines.mutableVector().resize(coordElt.size());

        for (size_t i = 0; i < coordElt.size(); ++i) {
            vector<S2Point> vertices;
            if (!parsePoints(coordElt[i].Array(), &vertices)) { return false; }
            out->lines.mutableVector()[i] = new S2Polyline();
            out->lines.mutableVector()[i]->Init(vertices);
        }

        return true;
    }

    bool GeoParser::isMultiPolygon(const BSONObj &obj) {
        BSONElement type = obj.getFieldDotted(GEOJSON_TYPE);
        if (type.eoo() || (String != type.type())) { return false; }
        if (GEOJSON_TYPE_MULTI_POLYGON != type.String()) { return false; }

        if (!crsIsOK(obj)) {
            warning() << "Invalid CRS: " << obj.toString() << endl;
            return false;
        }

        BSONElement coordElt = obj.getFieldDotted(GEOJSON_COORDINATES);
        if (coordElt.eoo() || (Array != coordElt.type())) { return false; }

        const vector<BSONElement>& coordinates = coordElt.Array();
        if (0 == coordinates.size()) { return false; }
        for (size_t i = 0; i < coordinates.size(); ++i) {
            if (coordinates[i].eoo() || (Array != coordinates[i].type())) { return false; }
            if (!isGeoJSONPolygonCoordinates(coordinates[i].Array())) { return false; }
        }

        return true;
    }

    bool GeoParser::parseMultiPolygon(const BSONObj &obj, MultiPolygonWithCRS *out) {
        vector<BSONElement> coordElt = obj.getFieldDotted(GEOJSON_COORDINATES).Array();
        out->polygons.mutableVector().clear();
        out->polygons.mutableVector().resize(coordElt.size());

        for (size_t i = 0; i < coordElt.size(); ++i) {
            out->polygons.mutableVector()[i] = new S2Polygon();
            if (!parseGeoJSONPolygonCoordinates(
                    coordElt[i].Array(), obj, out->polygons.vector()[i])) {
                return false;
            }
        }

        return true;
    }

    bool GeoParser::isGeometryCollection(const BSONObj &obj) {
        BSONElement type = obj.getFieldDotted(GEOJSON_TYPE);
        if (type.eoo() || (String != type.type())) { return false; }
        if (GEOJSON_TYPE_GEOMETRY_COLLECTION != type.String()) { return false; }

        BSONElement coordElt = obj.getFieldDotted(GEOJSON_GEOMETRIES);
        if (coordElt.eoo() || (Array != coordElt.type())) { return false; }

        const vector<BSONElement>& coordinates = coordElt.Array();
        if (0 == coordinates.size()) { return false; }

        for (size_t i = 0; i < coordinates.size(); ++i) {
            if (coordinates[i].eoo() || (Object != coordinates[i].type())) { return false; }
            BSONObj obj = coordinates[i].Obj();
            if (!isGeoJSONPoint(obj) && !isLine(obj) && !isGeoJSONPolygon(obj)
                && !isMultiPoint(obj) && !isMultiPolygon(obj) && !isMultiLine(obj)) {
                return false;
            }
        }

        return true;
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

    bool GeoParser::isCap(const BSONObj &obj) {
        return isLegacyCenter(obj) || isLegacyCenterSphere(obj);
    }

    bool GeoParser::parseCap(const BSONObj& obj, CapWithCRS *out) {
        if (isLegacyCenter(obj)) {
            BSONObjIterator typeIt(obj);
            BSONElement type = typeIt.next();
            BSONObjIterator objIt(type.embeddedObject());
            BSONElement center = objIt.next();
            if (!parseLegacyPoint(center.Obj(), &out->circle.center)) { return false; }
            BSONElement radius = objIt.next();
            out->circle.radius = radius.number();
            out->crs = FLAT;
        } else {
            BSONObjIterator typeIt(obj);
            BSONElement type = typeIt.next();
            BSONObjIterator objIt(type.embeddedObject());
            BSONObj centerObj = objIt.next().Obj();

            S2Point centerPoint;
            BSONObjIterator it(centerObj);
            BSONElement x = it.next();
            BSONElement y = it.next();
            centerPoint = coordToPoint(x.Number(), y.Number());
            BSONElement radiusElt = objIt.next();
            double radius = radiusElt.number();
            out->cap = S2Cap::FromAxisAngle(centerPoint, S1Angle::Radians(radius));
            out->crs = SPHERE;
        }
        return true;
    }

    bool GeoParser::parseGeometryCollection(const BSONObj &obj, GeometryCollection *out) {
        BSONElement coordElt = obj.getFieldDotted(GEOJSON_GEOMETRIES);
        const vector<BSONElement>& geometries = coordElt.Array();

        for (size_t i = 0; i < geometries.size(); ++i) {
            const BSONObj& geoObj = geometries[i].Obj();

            if (isGeoJSONPoint(geoObj)) {
                PointWithCRS point;
                if (!parsePoint(geoObj, &point)) { return false; }
                out->points.push_back(point);
            } else if (isLine(geoObj)) {
                out->lines.mutableVector().push_back(new LineWithCRS());
                if (!parseLine(geoObj, out->lines.vector().back())) { return false; }
            } else if (isGeoJSONPolygon(geoObj)) {
                out->polygons.mutableVector().push_back(new PolygonWithCRS());
                if (!parsePolygon(geoObj, out->polygons.vector().back())) { return false; }
            } else if (isMultiPoint(geoObj)) {
                out->multiPoints.mutableVector().push_back(new MultiPointWithCRS());
                if (!parseMultiPoint(geoObj, out->multiPoints.mutableVector().back())) {
                    return false;
                }
            } else if (isMultiPolygon(geoObj)) {
                out->multiPolygons.mutableVector().push_back(new MultiPolygonWithCRS());
                if (!parseMultiPolygon(geoObj, out->multiPolygons.mutableVector().back())) {
                    return false;
                }
            } else {
                verify(isMultiLine(geoObj));
                out->multiLines.mutableVector().push_back(new MultiLineWithCRS());
                if (!parseMultiLine(geoObj, out->multiLines.mutableVector().back())) {
                    return false;
                }
            }
        }

        return true;
    }

}  // namespace mongo
