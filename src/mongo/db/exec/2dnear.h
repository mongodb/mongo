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

#include "mongo/db/diskloc.h"
#include "mongo/db/exec/2dcommon.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/geo/geoquery.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"

namespace mongo {

    struct TwoDNearParams {
        NearQuery nearQuery;
        string ns;
        BSONObj indexKeyPattern;
        MatchExpression* filter;
        int numWanted;
        bool addPointMeta;
        bool addDistMeta;
    };

    struct Result {
        Result(WorkingSetID wsid, double dist) : id(wsid), distance(dist) { }

        bool operator<(const Result& other) const {
            // We want increasing distance, not decreasing, so we reverse the <.
            return distance > other.distance;
        }

        WorkingSetID id;

        double distance;
    };

    class TwoDNear : public PlanStage {
    public:
        TwoDNear(const TwoDNearParams& params, WorkingSet* ws);
        virtual ~TwoDNear();

        virtual bool isEOF();
        virtual StageState work(WorkingSetID* out);

        virtual void prepareToYield();
        virtual void recoverFromYield();
        virtual void invalidate(const DiskLoc& dl, InvalidationType type);

        virtual PlanStageStats* getStats();

    private:
        WorkingSet* _workingSet;

        // Stats
        CommonStats _commonStats;
        TwoDNearStats _specificStats;

        // We compute an annulus of results and cache it here.
        priority_queue<Result> _results;

        // For fast invalidation.  Perhaps not worth it.
        //
        // Multi-location docs mean that this is not one diskloc -> one WSID but one DiskLoc -> many
        // WSIDs.
        multimap<DiskLoc, WorkingSetID> _invalidationMap;

        TwoDNearParams _params;

        bool _initted;
    };

}  // namespace mongo

namespace mongo {
namespace twod_exec {

    class GeoHopper : public GeoBrowse {
    public:
        typedef multiset<GeoPoint> Holder;

        GeoHopper(TwoDAccessMethod* accessMethod,
                  unsigned max,
                  const Point& n,
                  MatchExpression* filter,
                  double maxDistance = numeric_limits<double>::max(),
                  GeoDistType type = GEO_PLANE);

        virtual KeyResult approxKeyCheck(const Point& p, double& d);

        virtual bool exactDocCheck(const Point& p, double& d);

        // Always in distance units, whether radians or normal
        double farthest() const { return _farthest; }

        virtual int addSpecific(const GeoIndexEntry& node, const Point& keyP, bool onBounds,
                                double keyD, bool potentiallyNewDoc);

        // Removes extra points from end of _points set.
        // Check can be a bit costly if we have lots of exact points near borders,
        // so we'll do this every once and awhile.
        void processExtraPoints();

        unsigned _max;
        Point _near;
        Holder _points;
        double _maxDistance;
        GeoDistType _type;
        double _distError;
        double _farthest;

        // Safe to use currently since we don't yield in $near searches.  If we do start to yield,
        // we may need to replace dirtied disklocs in our holder / ensure our logic is correct.
        map<DiskLoc, Holder::iterator> _seenPts;
    };

    class GeoSearch : public GeoHopper {
    public:
        GeoSearch(TwoDAccessMethod* accessMethod,
                  const Point& startPt,
                  int numWanted = 100,
                  MatchExpression* filter = NULL,
                  double maxDistance = numeric_limits<double>::max(),
                  GeoDistType type = GEO_PLANE);

        void exec();

        void addExactPoints(const GeoPoint& pt, Holder& points, bool force);

        void addExactPoints(const GeoPoint& pt, Holder& points, int& before, int& after,
                            bool force);

        // TODO: Refactor this back into holder class, allow to run periodically when we are seeing
        // a lot of pts
        void expandEndPoints(bool finish = true);

        virtual GeoHash expandStartHash() { return _start; }

        // Whether the current box width is big enough for our search area
        virtual bool fitsInBox(double width) { return width >= _scanDistance; }

        // Whether the current box overlaps our search area
        virtual double intersectsBox(Box& cur) { return cur.intersects(_want); }

        set< pair<DiskLoc,int> > _seen;
        GeoHash _start;
        int _numWanted;
        double _scanDistance;
        long long _nscanned;
        int _found;
        GeoDistType _type;
        Box _want;
        TwoDIndexingParams& _params;
    };

}  // namespace twod_exec
}  // namespace mongo
