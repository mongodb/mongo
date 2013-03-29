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
    // We assume that if you're trying to parse something, you know it's valid.
    class GeoParser {
    public:
        static bool isPoint(const BSONObj &obj);
        static void parsePoint(const BSONObj &obj, PointWithCRS *out);

        static bool isLine(const BSONObj &obj);
        static void parseLine(const BSONObj &obj, LineWithCRS *out);

        static bool isBox(const BSONObj &obj);
        static void parseBox(const BSONObj &obj, BoxWithCRS *out);

        static bool isPolygon(const BSONObj &obj);
        static void parsePolygon(const BSONObj &obj, PolygonWithCRS *out);

        // AKA $center or $centerSphere
        static bool isCap(const BSONObj &obj);
        static void parseCap(const BSONObj &obj, CapWithCRS *out);

        static bool isMultiPoint(const BSONObj &obj);
        static void parseMultiPoint(const BSONObj &obj, MultiPointWithCRS *out);

        static bool isMultiLine(const BSONObj &obj);
        static void parseMultiLine(const BSONObj &obj, MultiLineWithCRS *out);

        static bool isMultiPolygon(const BSONObj &obj);
        static void parseMultiPolygon(const BSONObj &obj, MultiPolygonWithCRS *out);

        static bool isGeometryCollection(const BSONObj &obj);
        static void parseGeometryCollection(const BSONObj &obj, GeometryCollection *out);

        // Return true if the CRS field is 1. missing, or 2. is well-formed and
        // has a datum we accept.  Otherwise, return false.
        // NOTE(hk): If this is ever used anywhere but internally, consider
        // returning states: missing, invalid, unknown, ok, etc. -- whatever
        // needed.
        static bool crsIsOK(const BSONObj& obj);
    };

}  // namespace mongo
