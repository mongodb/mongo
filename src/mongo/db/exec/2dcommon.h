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

#include "mongo/db/exec/index_scan.h"
#include "mongo/db/geo/core.h"
#include "mongo/db/geo/hash.h"
#include "mongo/db/geo/shapes.h"

#include "mongo/db/index/2d_access_method.h"

#pragma once

namespace mongo {
namespace twod_exec {

    //
    // Data structures
    //

    enum GeoDistType {
        GEO_PLANE,
        GEO_SPHERE
    };

    class GeoIndexEntry { 
    public:
        GeoIndexEntry(DiskLoc r, BSONObj k) : recordLoc(r), _key(k) { }
        const DiskLoc recordLoc;
        const BSONObj _key;
    private:
        GeoIndexEntry();
    };

    class GeoPoint {
    public:
        GeoPoint() : _distance(-1), _exact(false) { }

        //// Distance not used ////

        GeoPoint(const GeoIndexEntry& node, const BSONObj& obj)
            : _key(node._key), _loc(node.recordLoc), _o(obj),
              _distance(-1), _exact(false) { }

        //// Immediate initialization of distance ////

        GeoPoint(const GeoIndexEntry& node, const BSONObj& obj, double distance, bool exact)
            : _key(node._key), _loc(node.recordLoc), _o(obj),
              _distance(distance), _exact(exact) { }

        GeoPoint(const GeoPoint& pt, double distance, bool exact)
            : _key(pt.key()), _loc(pt.loc()), _o(pt.obj()), _distance(distance), _exact(exact) { }

        bool operator<(const GeoPoint& other) const {
            if (_distance != other._distance) return _distance < other._distance;
            if (_exact != other._exact) return _exact < other._exact;
            return _loc < other._loc;
        }

        double distance() const { return _distance; }
        bool isExact() const { return _exact; }
        BSONObj key() const { return _key; }
        bool hasLoc() const { return _loc.isNull(); }
        BSONObj obj() const { return _o; }
        BSONObj pt() const { return _pt; }
        bool isEmpty() { return _o.isEmpty(); }

        DiskLoc loc() const {
            return _loc;
        }

        std::string toString() const {
            return str::stream() << "Point from " << _key << " - " << _o
                                 << " dist : " << _distance << (_exact ? " (ex)" : " (app)");
        }

        BSONObj _key;
        DiskLoc _loc;
        BSONObj _o;
        BSONObj _pt;

        double _distance;
        bool _exact;

        BSONObj _id;
    };

    struct BtreeLocation {
        BtreeLocation() : _eof(false) { }

        scoped_ptr<IndexScan> _scan;
        scoped_ptr<WorkingSet> _ws;
        DiskLoc _loc;
        BSONObj _key;
        bool _eof;

        bool eof() { return _eof; }

        static bool hasPrefix(const BSONObj& key, const GeoHash& hash);

        void advance();

        void prepareToYield() { _scan->prepareToYield(); }
        void recoverFromYield() { _scan->recoverFromYield(); }

        // Returns the min and max keys which bound a particular location.
        // The only time these may be equal is when we actually equal the location
        // itself, otherwise our expanding algorithm will fail.
        static bool initial(const IndexDescriptor* descriptor, const TwoDIndexingParams& params,
                            BtreeLocation& min, BtreeLocation& max, GeoHash start);
    };

    //
    // Execution
    //

    class GeoAccumulator {
    public:
        GeoAccumulator(Collection* collection,
                       TwoDAccessMethod* accessMethod, MatchExpression* filter);

        virtual ~GeoAccumulator();

        enum KeyResult { BAD, BORDER, GOOD };

        virtual void add(const GeoIndexEntry& node);

        long long found() const { return _found; }

        virtual void getPointsFor(const BSONObj& key, const BSONObj& obj,
                                  std::vector<BSONObj> &locsForNode, bool allPoints = false);

        virtual int addSpecific(const GeoIndexEntry& node, const Point& p, bool inBounds, double d,
                                bool newDoc) = 0;

        virtual KeyResult approxKeyCheck(const Point& p, double& keyD) = 0;

        Collection* _collection;
        TwoDAccessMethod* _accessMethod;
        shared_ptr<GeoHashConverter> _converter;
        std::map<DiskLoc, bool> _matched;

        MatchExpression* _filter;

        long long _lookedAt;
        long long _matchesPerfd;
        long long _objectsLoaded;
        long long _pointsLoaded;
        long long _found;
    };

    class GeoBrowse : public GeoAccumulator {
    public:
        // The max points which should be added to an expanding box at one time
        static const int maxPointsHeuristic = 50;

        // Expand states
        enum State {
            START,
            DOING_EXPAND,
            DONE_NEIGHBOR,
            DONE
        } _state;

        GeoBrowse(Collection* collection,
                  TwoDAccessMethod* accessMethod,
                  std::string type,
                  MatchExpression* filter);

        virtual bool ok();
        virtual bool advance();
        virtual void noteLocation();

        /* called before query getmore block is iterated */
        virtual void checkLocation();

        virtual BSONObj current();
        virtual DiskLoc currLoc();
        virtual BSONObj currKey() const;

        // Are we finished getting points?
        virtual bool moreToDo();

        // Fills the stack, but only checks a maximum number of maxToCheck points at a time.
        // Further calls to this function will continue the expand/check neighbors algorithm.
        virtual void fillStack(int maxToCheck, int maxToAdd = -1, bool onlyExpand = false);

        bool checkAndAdvance(BtreeLocation* bl, const GeoHash& hash, int& totalFound);

        // The initial geo hash box for our first expansion
        virtual GeoHash expandStartHash() = 0;

        // Whether the current box width is big enough for our search area
        virtual bool fitsInBox(double width) = 0;

        // The amount the current box overlaps our search area
        virtual double intersectsBox(Box& cur) = 0;

        virtual bool exactDocCheck(const Point& p, double& d) = 0;

        bool remembered(BSONObj o);

        virtual int addSpecific(const GeoIndexEntry& node, const Point& keyP, bool onBounds,
                                double keyD, bool potentiallyNewDoc);

        virtual long long nscanned();

        virtual void explainDetails(BSONObjBuilder& b);

        void notePrefix() { _expPrefixes.push_back(_prefix); }

        /**
         * Returns true if the result was actually invalidated, false otherwise.
         */
        bool invalidate(const DiskLoc& dl);

        std::string _type;
        std::list<GeoPoint> _stack;
        std::set<BSONObj> _seenIds;

        GeoPoint _cur;
        bool _firstCall;

        long long _nscanned;

        // The current box we're expanding (-1 is first/center box)
        int _neighbor;

        // The points we've found so far
        int _foundInExp;

        // The current hash prefix we're expanding and the center-box hash prefix
        GeoHash _prefix;
        shared_ptr<GeoHash> _lastPrefix;
        GeoHash _centerPrefix;
        std::list<std::string> _fringe;
        int recurseDepth;
        Box _centerBox;

        // Start and end of our search range in the current box
        BtreeLocation _min;
        BtreeLocation _max;

        shared_ptr<GeoHash> _expPrefix;
        mutable std::vector<GeoHash> _expPrefixes;
        const IndexDescriptor* _descriptor;
        shared_ptr<GeoHashConverter> _converter;
        TwoDIndexingParams _params;

    private:
        const Collection* _collection;
    };

}  // namespace twod_exec
}  // namespace mongo
