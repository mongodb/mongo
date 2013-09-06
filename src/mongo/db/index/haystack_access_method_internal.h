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

#include <vector>

#include "mongo/db/diskloc.h"
#include "mongo/db/geo/core.h"
#include "mongo/db/geo/shapes.h"

namespace mongo {

    class GeoHaystackSearchHopper {
    public:
        /**
         * Constructed with a point, a max distance from that point, and a max number of
         * matched points to store.
         * @param n  The centroid that we're searching
         * @param maxDistance  The maximum distance to consider from that point
         * @param limit  The maximum number of results to return
         * @param geoField  Which field in the provided DiskLoc has the point to test.
         */
        GeoHaystackSearchHopper(const BSONObj& nearObj, double maxDistance, unsigned limit,
                                const string& geoField)
            : _near(nearObj), _maxDistance(maxDistance), _limit(limit), _geoField(geoField) { }

        // Consider the point in loc, and keep it if it's within _maxDistance (and we have space for
        // it)
        void consider(const DiskLoc& loc) {
            if (limitReached()) return;
            Point p(loc.obj().getFieldDotted(_geoField));
            if (distance(_near, p) > _maxDistance)
                return;
            _locs.push_back(loc);
        }

        int appendResultsTo(BSONArrayBuilder* b) {
            for (unsigned i = 0; i <_locs.size(); i++)
                b->append(_locs[i].obj());
            return _locs.size();
        }

        // Have we stored as many points as we can?
        bool limitReached() const {
            return _locs.size() >= _limit;
        }
    private:
        Point _near;
        double _maxDistance;
        unsigned _limit;
        const string _geoField;
        vector<DiskLoc> _locs;
    };

}  // namespace mongo
