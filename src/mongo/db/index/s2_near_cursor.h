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

#include <queue>

#include "mongo/db/btreecursor.h"
#include "mongo/db/geo/geoquery.h"
#include "mongo/db/geo/s2common.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pdfile.h"
#include "mongo/platform/unordered_set.h"
#include "third_party/s2/s2cap.h"
#include "third_party/s2/s2regionintersection.h"

namespace mongo {

    class S2NearIndexCursor : public IndexCursor {
    public:
        S2NearIndexCursor(IndexDescriptor* descriptor, const S2IndexingParams& params);

        virtual ~S2NearIndexCursor();

        // Not implemented.
        virtual Status seek(const BSONObj &position);
        Status setOptions(const CursorOptions& options);

        // Implemented:
        // This is our very specific seek function.  Not part of IndexCursor.
        void seek(const BSONObj& query, const NearQuery& nearQuery,
                  const vector<GeoQuery>& regions);
        virtual bool isEOF() const;
        virtual BSONObj getKey() const;
        virtual DiskLoc getValue() const;
        virtual void next();
        virtual string toString();
        virtual Status savePosition();
        virtual Status restorePosition();

        // The geoNear command wants these.
        long long nscanned() { return _stats._nscanned; }
        double currentDistance() { return _results.top().distance; }

    private:
        // We use this to cache results of the search.  Results are sorted to have decreasing
        // distance, and callers are interested in loc and key.
        struct Result {
            Result(const DiskLoc& dl, const BSONObj& ck, double dist) : loc(dl), key(ck),
                                                                        distance(dist) { }
            bool operator<(const Result& other) const {
                // We want increasing distance, not decreasing, so we reverse the <.
                return distance > other.distance;
            }

            DiskLoc loc;
            BSONObj key;
            double distance;
        };

        /**
         * Make the object that describes all keys that are within our current search annulus.
         */
        BSONObj makeFRSObject();

        /**
         * Fill _results with all of the results in the annulus defined by _innerRadius and
         * _outerRadius.  If no results are found, grow the annulus and repeat until success (or
         * until the edge of the world).
         */
        void fillResults();

        /**
         * Grow _innerRadius and _outerRadius by _radiusIncrement, capping _outerRadius at halfway
         * around the world (pi * _params.radius).
         */
        void nextAnnulus();

        double distanceTo(const BSONObj &obj);

        IndexDescriptor* _descriptor;

        // How we need/use the query:
        // Matcher: Can have geo fields in it, but only with $within.
        //          This only really happens (right now) from geoNear command.
        //          We assume the caller takes care of this in the right way.
        // FRS:     No geo fields allowed!
        // So, on that note: the query with the geo stuff taken out, used by makeFRSObject().
        BSONObj _filteredQuery;

        // The GeoQuery for the point we're doing near searching from.
        NearQuery _nearQuery;

        // What geo regions are we looking for?
        vector<GeoQuery> _indexedGeoFields;

        // How were the keys created?  We need this to search for the right stuff.
        S2IndexingParams _params;

        // We also pass this to the FieldRangeVector ctor.
        BSONObj _specForFRV;

        // Geo-related variables.
        // At what min distance (arc length) do we start looking for results?
        double _minDistance;
        // What's the max distance (arc length) we're willing to look for results?
        double _maxDistance;

        // We compute an annulus of results and cache it here.
        priority_queue<Result> _results;

        // These radii define the annulus we're currently looking at.
        double _innerRadius;
        double _outerRadius;

        // When we search the next annulus, what to adjust our radius by?  Grows when we search an
        // annulus and find no results.

        double _radiusIncrement;

        // What have we returned already?
        unordered_set<DiskLoc, DiskLoc::Hasher> _returned;

        struct Stats {
            Stats() : _nscanned(0), _matchTested(0), _geoMatchTested(0), _numShells(0),
                      _keyGeoSkip(0), _returnSkip(0), _btreeDups(0), _inAnnulusTested(0),
                      _numReturned(0) {}
            // Stat counters/debug information goes below.
            // How many items did we look at in the btree?
            long long _nscanned;
            // How many did we try to match?
            long long _matchTested;
            // How many did we geo-test?
            long long _geoMatchTested;
            // How many search shells did we use?
            long long _numShells;
            // How many did we skip due to key-geo check?
            long long _keyGeoSkip;
            long long _returnSkip;
            long long _btreeDups;
            long long _inAnnulusTested;
            long long _numReturned;
        };

        Stats _stats;

        // The S2 machinery that represents the search annulus
        S2Cap _innerCap;
        S2Cap _outerCap;
        S2RegionIntersection _annulus;

        // This is the "array index" of the key field that is the near field.  We use this to do
        // cheap is-this-doc-in-the-annulus testing.
        int _nearFieldIndex;

        // The max distance we've returned so far.
        double _returnedDistance;
    };

}  // namespace mongo
