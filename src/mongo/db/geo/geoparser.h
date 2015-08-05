/**
*    Copyright (C) 2013 10gen Inc.
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

#pragma once

#include "mongo/db/geo/shapes.h"
#include "mongo/db/jsobj.h"

namespace mongo {

// This class parses geographic data.
// It parses a subset of GeoJSON and creates S2 shapes from it.
// See http://geojson.org/geojson-spec.html for the spec.
//
// This class also parses the ad-hoc geo formats that MongoDB introduced.
//
// parse methods where validation is time consuming optimize to skip
// validation if the BSONObj was previously validated.
class GeoParser {
public:
    // Geospatial specifier after $geoWithin / $geoIntersects predicates.
    // i.e. "$box" in { $box: [[1, 2], [3, 4]] }
    enum GeoSpecifier {
        UNKNOWN = 0,
        BOX,            // $box
        CENTER,         // $center
        POLYGON,        // $polygon
        CENTER_SPHERE,  // $centerSphere
        GEOMETRY        // GeoJSON geometry, $geometry
    };

    // GeoJSON type defined in GeoJSON document.
    // i.e. "Point" in { type: "Point", coordinates: [1, 2] }
    enum GeoJSONType {
        GEOJSON_UNKNOWN = 0,
        GEOJSON_POINT,
        GEOJSON_LINESTRING,
        GEOJSON_POLYGON,
        GEOJSON_MULTI_POINT,
        GEOJSON_MULTI_LINESTRING,
        GEOJSON_MULTI_POLYGON,
        GEOJSON_GEOMETRY_COLLECTION
    };

    static GeoSpecifier parseGeoSpecifier(const BSONElement& elem);
    static GeoJSONType parseGeoJSONType(const BSONObj& obj);

    // Legacy points can contain extra data as extra fields - these are valid to index
    // e.g. { x: 1, y: 1, z: 1 }
    static Status parseLegacyPoint(const BSONElement& elem,
                                   PointWithCRS* out,
                                   bool allowAddlFields = false);
    // Parse the BSON object after $box, $center, etc.
    static Status parseLegacyBox(const BSONObj& obj, BoxWithCRS* out);
    static Status parseLegacyCenter(const BSONObj& obj, CapWithCRS* out);
    static Status parseLegacyPolygon(const BSONObj& obj, PolygonWithCRS* out);
    static Status parseCenterSphere(const BSONObj& obj, CapWithCRS* out);
    static Status parseGeoJSONPolygon(const BSONObj& obj, bool skipValidation, PolygonWithCRS* out);
    static Status parseGeoJSONPoint(const BSONObj& obj, PointWithCRS* out);
    static Status parseGeoJSONLine(const BSONObj& obj, bool skipValidation, LineWithCRS* out);
    static Status parseMultiPoint(const BSONObj& obj, MultiPointWithCRS* out);
    static Status parseMultiLine(const BSONObj& obj, bool skipValidation, MultiLineWithCRS* out);
    static Status parseMultiPolygon(const BSONObj& obj,
                                    bool skipValidation,
                                    MultiPolygonWithCRS* out);
    static Status parseGeometryCollection(const BSONObj& obj,
                                          bool skipValidation,
                                          GeometryCollection* out);

    // For geo near
    static Status parseQueryPoint(const BSONElement& elem, PointWithCRS* out);
    static Status parseStoredPoint(const BSONElement& elem, PointWithCRS* out);
    static bool parsePointWithMaxDistance(const BSONObj& obj, PointWithCRS* out, double* maxOut);
};

}  // namespace mongo
