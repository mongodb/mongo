/**
*    Copyright (C) 2008-2012 10gen Inc.
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

#include "mongo/db/jsobj.h"
#include <vector>
#include "third_party/s2/s2.h"

class S2Polyline;
class S2Polygon;

namespace mongo {
    // This class parses a subset of GeoJSON and creates S2 shapes from it.
    // See http://geojson.org/geojson-spec.html for the spec.
    //
    // We assume that if you're trying to parse something, you know it's valid.
    // This means don't call parsePoint(x) unless you're sure isPoint(x).
    // Perhaps there should just be parsePoint that returns bool and calls isPoint itself?
    class GeoJSONParser {
    public:
        static bool isPoint(const BSONObj &obj);
        static void parsePoint(const BSONObj &obj, S2Point *out);

        static bool isLineString(const BSONObj &obj);
        static void parseLineString(const BSONObj &obj, S2Polyline *out);

        static bool isPolygon(const BSONObj &obj);
        static void parsePolygon(const BSONObj &obj, S2Polygon *out);
    };
}  // namespace mongo
