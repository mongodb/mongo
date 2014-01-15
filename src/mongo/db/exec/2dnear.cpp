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

#include "mongo/db/exec/2dnear.h"

#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/catalog/collection.h"

namespace mongo {

    TwoDNear::TwoDNear(const TwoDNearParams& params, WorkingSet* ws) {
        _params = params;
        _workingSet = ws;
        _initted = false;
    }

    TwoDNear::~TwoDNear() { }

    bool TwoDNear::isEOF() {
        return _initted && _results.empty();
    }

    PlanStage::StageState TwoDNear::work(WorkingSetID* out) {
        ++_commonStats.works;
        if (!_initted) {
            _initted = true;

            Database* db = cc().database();
            if ( !db )
                return PlanStage::IS_EOF;
            Collection* collection = db->getCollection( _params.ns );
            if ( !collection )
                return PlanStage::IS_EOF;

            IndexDescriptor* desc = collection->getIndexCatalog()->findIndexByKeyPattern(_params.indexKeyPattern);
            if ( desc == NULL )
                return PlanStage::IS_EOF;
            TwoDAccessMethod* am = static_cast<TwoDAccessMethod*>( collection->getIndexCatalog()->getIndex( desc ) );

            auto_ptr<twod_exec::GeoSearch> search;
            search.reset(new twod_exec::GeoSearch(am,
                                           _params.nearQuery.centroid.oldPoint,
                                           _params.numWanted, 
                                           _params.filter,
                                           _params.nearQuery.maxDistance,
                                           _params.nearQuery.isNearSphere ? twod_exec::GEO_SPHERE
                                                                          : twod_exec::GEO_PLANE,
                                           _params.nearQuery.uniqueDocs,
                                           false));

            // This is where all the work is done.  :(
            search->exec();
            _specificStats.objectsLoaded = search->_objectsLoaded;
            _specificStats.nscanned = search->_nscanned;

            for (twod_exec::GeoHopper::Holder::iterator it = search->_points.begin();
                 it != search->_points.end(); it++) {

                WorkingSetID id = _workingSet->allocate();
                WorkingSetMember* member = _workingSet->get(id);
                member->loc = it->_loc;
                member->obj = member->loc.obj();
                member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;
                if (_params.addDistMeta) {
                    member->addComputed(new GeoDistanceComputedData(it->_distance));
                }
                if (_params.addPointMeta) {
                    member->addComputed(new GeoNearPointComputedData(it->_pt));
                }
                _results.push(Result(id, it->_distance));
                _invalidationMap.insert(pair<DiskLoc, WorkingSetID>(it->_loc, id));
            }
        }

        if (isEOF()) { return PlanStage::IS_EOF; }

        Result result = _results.top();
        _results.pop();
        *out = result.id;

        // Remove from invalidation map.
        WorkingSetMember* member = _workingSet->get(*out);

        // The WSM may have been mutated or deleted so it may not have a loc.
        if (member->hasLoc()) {
            typedef multimap<DiskLoc, WorkingSetID>::iterator MMIT;
            pair<MMIT, MMIT> range = _invalidationMap.equal_range(member->loc);
            for (MMIT it = range.first; it != range.second; ++it) {
                if (it->second == *out) {
                    _invalidationMap.erase(it);
                    break;
                }
            }
        }

        ++_commonStats.advanced;
        return PlanStage::ADVANCED;
    }

    void TwoDNear::prepareToYield() {
        // Nothing to do here.
    }

    void TwoDNear::recoverFromYield() {
        // Also nothing to do here.
    }

    void TwoDNear::invalidate(const DiskLoc& dl, InvalidationType type) {
        // We do the same thing for mutation or deletion: fetch the doc and forget about the
        // DiskLoc.  2d's near search computes all its results in one go so we know that we'll still
        // return valid data.
        typedef multimap<DiskLoc, WorkingSetID>::iterator MMIT;
        pair<MMIT, MMIT> range = _invalidationMap.equal_range(dl);
        for (MMIT it = range.first; it != range.second; ++it) {
            WorkingSetMember* member = _workingSet->get(it->second);
            // If it's in the invalidation map it must have a DiskLoc.
            verify(member->hasLoc());
            WorkingSetCommon::fetchAndInvalidateLoc(member);
            verify(!member->hasLoc());
        }
        _invalidationMap.erase(range.first, range.second);
    }

    PlanStageStats* TwoDNear::getStats() {
        _commonStats.isEOF = isEOF();
        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_GEO_NEAR_2D));
        ret->specific.reset(new TwoDNearStats(_specificStats));
        return ret.release();
    }

}  // namespace mongo

namespace mongo {
namespace twod_exec {

    //
    // GeoHopper
    //

    GeoHopper::GeoHopper(TwoDAccessMethod* accessMethod,
            unsigned max,
            const Point& n,
            MatchExpression* filter,
            double maxDistance,
            GeoDistType type,
            bool uniqueDocs,
            bool needDistance)
        : GeoBrowse(accessMethod, "search", filter, uniqueDocs, needDistance),
        _max(max),
        _near(n),
        _maxDistance(maxDistance),
        _type(type),
        _distError(type == GEO_PLANE
                ? accessMethod->getParams().geoHashConverter->getError()
                : accessMethod->getParams().geoHashConverter->getErrorSphere()),
        _farthest(0) { }

    GeoAccumulator:: KeyResult GeoHopper::approxKeyCheck(const Point& p, double& d) {
        // Always check approximate distance, since it lets us avoid doing
        // checks of the rest of the object if it succeeds
        switch (_type) {
            case GEO_PLANE:
                d = distance(_near, p);
                break;
            case GEO_SPHERE:
                checkEarthBounds(p);
                d = spheredist_deg(_near, p);
                break;
            default: verify(false);
        }
        verify(d >= 0);

        // If we need more points
        double borderDist = (_points.size() < _max ? _maxDistance : farthest());

        if (d >= borderDist - 2 * _distError && d <= borderDist + 2 * _distError) return BORDER;
        else return d < borderDist ? GOOD : BAD;
    }

    bool GeoHopper::exactDocCheck(const Point& p, double& d){
        bool within = false;

        // Get the appropriate distance for the type
        switch (_type) {
            case GEO_PLANE:
                d = distance(_near, p);
                within = distanceWithin(_near, p, _maxDistance);
                break;
            case GEO_SPHERE:
                checkEarthBounds(p);
                d = spheredist_deg(_near, p);
                within = (d <= _maxDistance);
                break;
            default: verify(false);
        }

        return within;
    }


    int GeoHopper::addSpecific(const GeoIndexEntry& node, const Point& keyP, bool onBounds,
            double keyD, bool potentiallyNewDoc) {
        // Unique documents
        GeoPoint newPoint(node, keyD, false);
        int prevSize = _points.size();

        //cout << "uniquedocs: " << _uniqueDocs << endl;

        // STEP 1 : Remove old duplicate points from the set if needed
        if(_uniqueDocs){
            // Lookup old point with same doc
            map<DiskLoc, Holder::iterator>::iterator oldPointIt = _seenPts.find(newPoint.loc());

            if(oldPointIt != _seenPts.end()){
                const GeoPoint& oldPoint = *(oldPointIt->second);
                // We don't need to care if we've already seen this same approx pt or better,
                // or we've already gone to disk once for the point
                if(oldPoint < newPoint){
                    return 0;
                }
                _points.erase(oldPointIt->second);
            }
        }

        //cout << "inserting point\n";
        Holder::iterator newIt = _points.insert(newPoint);
        if(_uniqueDocs) _seenPts[ newPoint.loc() ] = newIt;

        verify(_max > 0);

        Holder::iterator lastPtIt = _points.end();
        lastPtIt--;
        _farthest = lastPtIt->distance() + 2 * _distError;
        return _points.size() - prevSize;
    }

    // Removes extra points from end of _points set.
    // Check can be a bit costly if we have lots of exact points near borders,
    // so we'll do this every once and awhile.
    void GeoHopper::processExtraPoints(){
        if(_points.size() == 0) return;
        int prevSize = _points.size();

        // Erase all points from the set with a position >= _max *and*
        // whose distance isn't close to the _max - 1 position distance
        int numToErase = _points.size() - _max;
        if(numToErase < 0) numToErase = 0;

        // Get the first point definitely in the _points array
        Holder::iterator startErase = _points.end();
        for(int i = 0; i < numToErase + 1; i++) startErase--;
        _farthest = startErase->distance() + 2 * _distError;

        startErase++;
        while(numToErase > 0 && startErase->distance() <= _farthest){
            numToErase--;
            startErase++;
            verify(startErase != _points.end() || numToErase == 0);
        }

        if(_uniqueDocs){
            for(Holder::iterator i = startErase; i != _points.end(); ++i)
                _seenPts.erase(i->loc());
        }

        _points.erase(startErase, _points.end());

        int diff = _points.size() - prevSize;
        if(diff > 0) _found += diff;
        else _found -= -diff;
    }

    //
    // GeoSearch
    //

    GeoSearch::GeoSearch(TwoDAccessMethod* accessMethod,
            const Point& startPt,
            int numWanted,
            MatchExpression* filter,
            double maxDistance,
            GeoDistType type,
            bool uniqueDocs,
            bool needDistance)
        : GeoHopper(accessMethod, numWanted, startPt, filter, maxDistance, type,
                uniqueDocs, needDistance),
        _start(accessMethod->getParams().geoHashConverter->hash(startPt.x, startPt.y)),
        _numWanted(numWanted),
        _type(type),
        _params(accessMethod->getParams()) {

            _nscanned = 0;
            _found = 0;

            if(_maxDistance < 0){
                _scanDistance = numeric_limits<double>::max();
            } else if (type == GEO_PLANE) {
                _scanDistance = maxDistance + _params.geoHashConverter->getError();
            } else if (type == GEO_SPHERE) {
                checkEarthBounds(startPt);
                // TODO: consider splitting into x and y scan distances
                _scanDistance = computeXScanDistance(startPt.y,
                        rad2deg(_maxDistance) + _params.geoHashConverter->getError());
            }

            verify(_scanDistance > 0);
        }

    void GeoSearch::exec() {
        if(_numWanted == 0) return;

        /*
         * Search algorithm
         * 1) use geohash prefix to find X items
         * 2) compute max distance from want to an item
         * 3) find optimal set of boxes that complete circle
         * 4) use regular btree cursors to scan those boxes
         */

        // Part 1
        {
            do {
                long long f = found();
                verify(f <= 0x7fffffff);
                fillStack(maxPointsHeuristic, _numWanted - static_cast<int>(f), true);
                processExtraPoints();
            } while(_state != DONE && _state != DONE_NEIGHBOR &&
                    found() < _numWanted &&
                    (!_prefix.constrains() ||
                     _params.geoHashConverter->sizeEdge(_prefix) <= _scanDistance));

            // If we couldn't scan or scanned everything, we're done
            if(_state == DONE){
                expandEndPoints();
                return;
            }
        }

        // Part 2
        {
            // Find farthest distance for completion scan
            double farDist = farthest();
            if(found() < _numWanted) {
                // Not enough found in Phase 1
                farDist = _scanDistance;
            }
            else if (_type == GEO_PLANE) {
                // Enough found, but need to search neighbor boxes
                farDist += _params.geoHashConverter->getError();
            }
            else if (_type == GEO_SPHERE) {
                // Enough found, but need to search neighbor boxes
                farDist = std::min(_scanDistance,
                        computeXScanDistance(_near.y,
                            rad2deg(farDist))
                        + 2 * _params.geoHashConverter->getError());
            }
            verify(farDist >= 0);

            // Find the box that includes all the points we need to return
            _want = Box(_near.x - farDist, _near.y - farDist, farDist * 2);

            // Remember the far distance for further scans
            _scanDistance = farDist;

            // Reset the search, our distances have probably changed
            if(_state == DONE_NEIGHBOR){
                _state = DOING_EXPAND;
                _neighbor = -1;
            }

            // Do regular search in the full region
            do {
                fillStack(maxPointsHeuristic);
                processExtraPoints();
            }
            while(_state != DONE);
        }

        expandEndPoints();
    }

    void GeoSearch::addExactPoints(const GeoPoint& pt, Holder& points, bool force){
        int before, after;
        addExactPoints(pt, points, before, after, force);
    }

    void GeoSearch::addExactPoints(const GeoPoint& pt, Holder& points, int& before, int& after,
            bool force){
        before = 0;
        after = 0;

        if(pt.isExact()){
            if(force) points.insert(pt);
            return;
        }

        vector<BSONObj> locs;
        getPointsFor(pt.key(), pt.obj(), locs, _uniqueDocs);

        GeoPoint nearestPt(pt, -1, true);

        for(vector<BSONObj>::iterator i = locs.begin(); i != locs.end(); i++){
            Point loc(*i);
            double d;
            if(! exactDocCheck(loc, d)) continue;

            if(_uniqueDocs && (nearestPt.distance() < 0 || d < nearestPt.distance())){
                nearestPt._distance = d;
                nearestPt._pt = *i;
                continue;
            } else if(! _uniqueDocs){
                GeoPoint exactPt(pt, d, true);
                exactPt._pt = *i;
                points.insert(exactPt);
                exactPt < pt ? before++ : after++;
            }
        }

        if(_uniqueDocs && nearestPt.distance() >= 0){
            points.insert(nearestPt);
            if(nearestPt < pt) before++;
            else after++;
        }
    }

    // TODO: Refactor this back into holder class, allow to run periodically when we are seeing
    // a lot of pts
    void GeoSearch::expandEndPoints(bool finish) {
        processExtraPoints();
        // All points in array *could* be in maxDistance

        // Step 1 : Trim points to max size TODO:  This check will do little for now, but is
        // skeleton for future work in incremental $near
        // searches
        if(_max > 0){
            int numToErase = _points.size() - _max;
            if(numToErase > 0){
                Holder tested;
                // Work backward through all points we're not sure belong in the set
                Holder::iterator maybePointIt = _points.end();
                maybePointIt--;
                double approxMin = maybePointIt->distance() - 2 * _distError;

                // Insert all
                int erased = 0;
                while(_points.size() > 0
                        && (maybePointIt->distance() >= approxMin || erased < numToErase)){

                    Holder::iterator current = maybePointIt;
                    if (current != _points.begin())
                        --maybePointIt;

                    addExactPoints(*current, tested, true);
                    _points.erase(current);
                    erased++;

                    if(tested.size())
                        approxMin = tested.begin()->distance() - 2 * _distError;
                }

                int numToAddBack = erased - numToErase;
                verify(numToAddBack >= 0);

                Holder::iterator testedIt = tested.begin();
                for(int i = 0; i < numToAddBack && testedIt != tested.end(); i++){
                    _points.insert(*testedIt);
                    testedIt++;
                }
            }
        }

        // We've now trimmed first set of unneeded points

        // Step 2: iterate through all points and add as needed
        unsigned expandedPoints = 0;
        Holder::iterator it = _points.begin();
        double expandWindowEnd = -1;

        while(it != _points.end()){
            const GeoPoint& currPt = *it;
            // TODO: If one point is exact, maybe not 2 * _distError

            // See if we're in an expand window
            bool inWindow = currPt.distance() <= expandWindowEnd;
            // If we're not, and we're done with points, break
            if(! inWindow && expandedPoints >= _max) break;

            bool expandApprox = !currPt.isExact() && (!_uniqueDocs || finish || inWindow);

            if (expandApprox) {
                // Add new point(s). These will only be added in a radius of 2 * _distError
                // around the current point, so should not affect previously valid points.
                int before, after;
                addExactPoints(currPt, _points, before, after, false);
                expandedPoints += before;

                if(_max > 0 && expandedPoints < _max)
                    expandWindowEnd = currPt.distance() + 2 * _distError;

                // Iterate to the next point
                Holder::iterator current = it++;
                // Erase the current point
                _points.erase(current);
            } else{
                expandedPoints++;
                it++;
            }
        }

        // Finish
        // TODO:  Don't really need to trim?
        for(; expandedPoints > _max; expandedPoints--) it--;
        _points.erase(it, _points.end());
    }

}  // namespace twod_exec
}  // namespace mongo
