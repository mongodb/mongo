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

#include "mongo/db/diskloc.h"
#include "mongo/db/geo/geoparser.h"
#include "third_party/s2/s2.h"
#include "third_party/s2/s2regioncoverer.h"
#include "third_party/s2/s2cell.h"
#include "third_party/s2/s2polyline.h"
#include "third_party/s2/s2polygon.h"
#include "third_party/s2/s2regioncoverer.h"

#pragma once

namespace mongo {

    class S2SearchUtil {
    public:
        // Given a coverer, region, and field name, generate a BSONObj that we can pass to a
        // FieldRangeSet so that we only examine the keys that the provided region may intersect.
        static BSONObj coverAsBSON(const vector<S2CellId> &cover, const string& field,
                                   const int coarsestIndexedLevel);
        static void setCoverLimitsBasedOnArea(double area, S2RegionCoverer *coverer, int coarsestIndexedLevel);
    };

}  // namespace mongo
