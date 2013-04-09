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

#include "mongo/db/jsobj.h"

namespace mongo {
    // Arguments in common between 2d and 2dsphere geoNear.
    class GeoNearArguments {
    public:
        GeoNearArguments(const BSONObj& cmdObj);
        int numWanted;
        bool uniqueDocs;
        bool includeLocs;
        BSONObj query;
        double distanceMultiplier;
        bool isSpherical;
    private:
        GeoNearArguments() { }
    };
}  // namespace mongo
