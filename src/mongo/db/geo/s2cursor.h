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
#include "mongo/db/cursor.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/matcher.h"
#include "mongo/db/geo/geoquery.h"
#include "mongo/db/geo/s2common.h"

namespace mongo {

    class BtreeCursor;
    class IndexDetails;

    class S2Cursor : public Cursor {
    public:
        S2Cursor(const BSONObj &keyPattern, const IndexDetails* details, const BSONObj &query,
                 const vector<GeoQuery> &regions, const S2IndexingParams &params);
        virtual ~S2Cursor(); 
        virtual CoveredIndexMatcher *matcher() const;

        virtual bool supportYields()  { return true; }
        virtual bool supportGetMore() { return true; }
        virtual bool isMultiKey() const { return true; }
        virtual bool autoDedup() const { return false; }
        virtual bool modifiedKeys() const { return true; }
        virtual bool getsetdup(DiskLoc loc);
        virtual void aboutToDeleteBucket(const DiskLoc& b);
        virtual string toString() { return "S2Cursor"; }
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
    private:
        // Make an object that describes the restrictions on all possible valid keys.
        // It's kind of a monstrous object.  Thanks, FieldRangeSet, for doing all the work
        // for us.
        // Returns false if the FRS object would be empty.
        bool makeFRSObject(BSONObj *out);

        // Need this to make a FieldRangeSet.
        const IndexDetails *_details;
        // The query with the geo stuff taken out.  We use this with a matcher.
        BSONObj _filteredQuery;
        // What geo regions are we looking for?
        vector<GeoQuery> _fields;
        // We use this for matching non-GEO stuff.
        shared_ptr<CoveredIndexMatcher> _matcher;
        // How were the keys created?  We need this to search for the right stuff.
        S2IndexingParams _params;
        // We have to pass this to the FieldRangeVector ctor (in modified form).
        BSONObj _keyPattern;

        // What have we checked so we don't repeat it and waste time?
        set<DiskLoc> _seen;
        // This really does all the work/points into the btree.
        scoped_ptr<BtreeCursor> _btreeCursor;

        // Stat counters/debug information goes below.
        // How many items did we look at in the btree?
        long long _nscanned;
        // How many did we try to match?
        long long _matchTested;
        // How many did we geo-test?
        long long _geoTested;
        // How many cells were in our cover?
        long long _cellsInCover;
    };
}  // namespace mongo
