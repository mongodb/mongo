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

#include <vector>
#include "mongo/db/jsobj.h"
#include "mongo/db/commands.h"
#include "mongo/db/btreecursor.h"
#include "mongo/db/cursor.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/matcher.h"
#include "mongo/db/queryutil.h"
#include "mongo/db/geo/s2common.h"
#include "mongo/db/geo/geoquery.h"
#include "mongo/platform/unordered_set.h"
#include "third_party/s2/s2cap.h"
#include "third_party/s2/s2regionintersection.h"

namespace mongo {
    class S2NearCursor : public Cursor {
    public:
        S2NearCursor(const BSONObj &keyPattern, const IndexDetails* details, const BSONObj &query,
                     const NearQuery &nearQuery, const vector<GeoQuery> &indexedGeoRegions,
                     const S2IndexingParams &params);
        virtual ~S2NearCursor(); 
        virtual CoveredIndexMatcher *matcher() const;

        virtual bool supportYields()  { return true; }
        virtual bool supportGetMore() { return true; }
        virtual bool isMultiKey() const { return true; }
        virtual bool autoDedup() const { return false; }
        virtual bool modifiedKeys() const { return true; }
        virtual bool getsetdup(DiskLoc loc) { return false; }
        virtual string toString() { return "S2NearCursor"; }
        BSONObj indexKeyPattern() { return _keyPattern; }
        virtual bool ok();
        virtual Record* _current();
        virtual BSONObj current();
        virtual DiskLoc currLoc();
        virtual bool advance();
        virtual BSONObj currKey() const;
        virtual DiskLoc refLoc();
        virtual void noteLocation();
        virtual void checkLocation();
        virtual long long nscanned();
        virtual void explainDetails(BSONObjBuilder& b);

        double currentDistance() const;
    private:
        // We use this to cache results of the search.  Results are sorted to have decreasing
        // distance, and callers are interested in loc and key.
        struct Result {
            Result(const DiskLoc &dl, const BSONObj &ck, double dist) : loc(dl), key(ck),
                                                                        distance(dist) { }
            bool operator<(const Result& other) const {
                // We want increasing distance, not decreasing, so we reverse the <.
                return distance > other.distance;
            }
            DiskLoc loc;
            BSONObj key;
            double distance;
        };

        // Make the object that describes all keys that are within our current search annulus.
        BSONObj makeFRSObject();
        // Fill _results with all of the results in the annulus defined by _innerRadius and
        // _outerRadius.  If no results are found, grow the annulus and repeat until success (or
        // until the edge of the world).
        void fillResults();
        // Grow _innerRadius and _outerRadius by _radiusIncrement, capping _outerRadius at halfway
        // around the world (pi * _params.radius).
        void nextAnnulus();
        double distanceTo(const BSONObj &obj);

        // Need this to make a FieldRangeSet.
        const IndexDetails *_details;

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
        // We use this for matching non-GEO stuff.
        shared_ptr<CoveredIndexMatcher> _matcher;
        // How were the keys created?  We need this to search for the right stuff.
        S2IndexingParams _params;
        // We have to pass this to the FieldRangeVector ctor (in modified form).
        BSONObj _keyPattern;
        // We also pass this to the FieldRangeVector ctor.
        IndexSpec _specForFRV;

        // Geo-related variables.
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
