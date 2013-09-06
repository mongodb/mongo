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
    // parse* methods may do some more validation than the is* methods; they return false if they
    // encounter invalid geometry and true if the geometry is parsed successfully.
    class GeoParser {
    public:
        static bool isPoint(const BSONObj &obj);
        static bool parsePoint(const BSONObj &obj, PointWithCRS *out);

        static bool isLine(const BSONObj &obj);
        static bool parseLine(const BSONObj &obj, LineWithCRS *out);

        static bool isBox(const BSONObj &obj);
        static bool parseBox(const BSONObj &obj, BoxWithCRS *out);

        static bool isPolygon(const BSONObj &obj);
        static bool parsePolygon(const BSONObj &obj, PolygonWithCRS *out);

        // AKA $center or $centerSphere
        static bool isCap(const BSONObj &obj);
        static bool parseCap(const BSONObj &obj, CapWithCRS *out);

        static bool isMultiPoint(const BSONObj &obj);
        static bool parseMultiPoint(const BSONObj &obj, MultiPointWithCRS *out);

        static bool isMultiLine(const BSONObj &obj);
        static bool parseMultiLine(const BSONObj &obj, MultiLineWithCRS *out);

        static bool isMultiPolygon(const BSONObj &obj);
        static bool parseMultiPolygon(const BSONObj &obj, MultiPolygonWithCRS *out);

        static bool isGeometryCollection(const BSONObj &obj);
        static bool parseGeometryCollection(const BSONObj &obj, GeometryCollection *out);

        // Return true if the CRS field is 1. missing, or 2. is well-formed and
        // has a datum we accept.  Otherwise, return false.
        // NOTE(hk): If this is ever used anywhere but internally, consider
        // returning states: missing, invalid, unknown, ok, etc. -- whatever
        // needed.
        static bool crsIsOK(const BSONObj& obj);
    };

}  // namespace mongo
