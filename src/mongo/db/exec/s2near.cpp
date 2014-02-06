/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/db/exec/s2near.h"

#include "mongo/db/client.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/geo/geoconstants.h"
#include "mongo/db/index/expression_index.h"
#include "third_party/s2/s2regionintersection.h"

namespace mongo {

    S2NearStage::S2NearStage(const S2NearParams& params, WorkingSet* ws) {
        _initted = false;
        _params = params;
        _ws = ws;
        _worked = false;
        _failed = false;
    }

    void S2NearStage::init() {
        _initted = true;

        // The field we're near-ing from is the n-th field.  Figure out what that 'n' is.  We
        // put the cover for the search annulus in this spot in the bounds.
        _nearFieldIndex = 0;
        BSONObjIterator specIt(_params.indexKeyPattern);
        while (specIt.more()) {
             if (specIt.next().fieldName() == _params.nearQuery.field) {
                 break;
             }
            ++_nearFieldIndex;
        }

        verify(_nearFieldIndex < _params.indexKeyPattern.nFields());

        // FLAT implies the distances are in radians.  Convert to meters.
        if (FLAT == _params.nearQuery.centroid.crs) {
            _params.nearQuery.minDistance *= kRadiusOfEarthInMeters;
            _params.nearQuery.maxDistance *= kRadiusOfEarthInMeters;
        }

        // Make sure distances are sane.  Possibly redundant given the checking during parsing.
        _minDistance = max(0.0, _params.nearQuery.minDistance);
        _maxDistance = min(M_PI * kRadiusOfEarthInMeters, _params.nearQuery.maxDistance);
        _minDistance = min(_minDistance, _maxDistance);

        // We grow _outerRadius in nextAnnulus() below.
        _innerRadius = _outerRadius = _minDistance;
        _outerRadiusInclusive = false;

        // Grab the IndexDescriptor.
        Database* db = cc().database();
        if (!db) {
            _failed = true;
            return;
        }

        Collection* collection = db->getCollection(_params.ns);
        if (!collection) {
            _failed = true;
            return;
        }

        _descriptor = collection->getIndexCatalog()->findIndexByKeyPattern(_params.indexKeyPattern);
        if (NULL == _descriptor) {
            _failed = true;
            return;
        }

        // The user can override this so we honor it.  We could ignore it though -- it's just used
        // to set _radiusIncrement, not to do any covering.
        int finestIndexedLevel;
        BSONElement fl = _descriptor->infoObj()["finestIndexedLevel"];
        if (fl.isNumber()) {
            finestIndexedLevel = fl.numberInt();
        }
        else {
            finestIndexedLevel = S2::kAvgEdge.GetClosestLevel(500.0 / kRadiusOfEarthInMeters);
        }

        // Start with a conservative _radiusIncrement.  When we're done searching a shell we
        // increment the two radii by this.
        _radiusIncrement = 5 * S2::kAvgEdge.GetValue(finestIndexedLevel) * kRadiusOfEarthInMeters;
    }

    S2NearStage::~S2NearStage() {
        // _annulus temporarily takes ownership of some member variables.
        // Release them to avoid double-deleting _innerCap and _outerCap.
        _annulus.Release(NULL);
    }

    PlanStage::StageState S2NearStage::work(WorkingSetID* out) {
        if (!_initted) { init(); }

        if (_failed) {
            mongoutils::str::stream ss;
            ss << "unable to load geo index " << _params.indexKeyPattern;
            Status status(ErrorCodes::IndexNotFound, ss);
            *out = WorkingSetCommon::allocateStatusMember( _ws, status);
            return PlanStage::FAILURE;
        }
        if (isEOF()) { return PlanStage::IS_EOF; }
        ++_commonStats.works;

        // If we haven't opened up our very first ixscan+fetch children, do it.  This is kind of
        // heavy so we don't want to do it in the ctor.
        if (!_worked) {
            nextAnnulus();
            _worked = true;
        }

        // If we're still reading results from the child, do that.
        if (NULL != _child.get()) {
            return addResultToQueue(out);
        }

        // Not reading results.  Perhaps we're returning buffered results.
        if (!_results.empty()) {
            Result result = _results.top();
            _results.pop();
            *out = result.id;

            // Remove from invalidation map.
            WorkingSetMember* member = _ws->get(*out);
            if (member->hasLoc()) {
                unordered_map<DiskLoc, WorkingSetID, DiskLoc::Hasher>::iterator it
                    = _invalidationMap.find(member->loc);
                verify(_invalidationMap.end() != it);
                _invalidationMap.erase(it);
            }

            ++_commonStats.advanced;
            return PlanStage::ADVANCED;
        }

        // Not EOF, not reading results, not returning any buffered results.  Look in the next shell
        // for results.
        nextAnnulus();
        return PlanStage::NEED_TIME;
    }

    void S2NearStage::nextAnnulus() {
        // Step 1: Grow the annulus.
        _innerRadius = _outerRadius;
        _outerRadius += _radiusIncrement;
        if (_outerRadius >= _maxDistance) {
            _outerRadius = _maxDistance;
            _outerRadiusInclusive = true;
        }
        verify(_innerRadius <= _outerRadius);

        // We might have just grown our radius beyond anything reasonable.
        if (isEOF()) { return; }

        // Step 2: Fill out bounds for the ixscan we use.
        _innerCap = S2Cap::FromAxisAngle(_params.nearQuery.centroid.point,
                                         S1Angle::Radians(_innerRadius / kRadiusOfEarthInMeters));
        _outerCap = S2Cap::FromAxisAngle(_params.nearQuery.centroid.point,
                                         S1Angle::Radians(_outerRadius / kRadiusOfEarthInMeters));
        _innerCap = _innerCap.Complement();

        vector<S2Region*> regions;
        regions.push_back(&_innerCap);
        regions.push_back(&_outerCap);

        _annulus.Release(NULL);
        _annulus.Init(&regions);

        // Step 3: Actually create the ixscan.

        IndexScanParams params;
        params.descriptor = _descriptor;
        _params.baseBounds.fields[_nearFieldIndex].intervals.clear();
        ExpressionMapping::cover2dsphere(_annulus,
                                         params.descriptor->infoObj(),
                                         &_params.baseBounds.fields[_nearFieldIndex]);

        params.bounds = _params.baseBounds;
        params.direction = 1;
        IndexScan* scan = new IndexScan(params, _ws, NULL);

        // Owns 'scan'.
        _child.reset(new FetchStage(_ws, scan, _params.filter));
    }

    PlanStage::StageState S2NearStage::addResultToQueue(WorkingSetID* out) {
        PlanStage::StageState state = _child->work(out);

        // All done reading from _child.
        if (PlanStage::IS_EOF == state) {
            _child.reset();

            // Adjust the annulus size depending on how many results we got.
            if (_results.empty()) {
                _radiusIncrement *= 2;
            } else if (_results.size() < 300) {
                _radiusIncrement *= 2;
            } else if (_results.size() > 600) {
                _radiusIncrement /= 2;
            }

            // Make a new ixscan next time.
            return PlanStage::NEED_TIME;
        }

        // Nothing to do unless we advance.
        if (PlanStage::ADVANCED != state) { return state; }

        // TODO Speed improvements:
        //
        // 0. Modify fetch to preserve key data and test for intersection w/annulus.
        //
        // 1. keep track of what we've seen in this scan and possibly ignore it.
        //
        // 2. keep track of results we've returned before and ignore them.

        WorkingSetMember* member = _ws->get(*out);
        // Must have an object in order to get geometry out of it.
        verify(member->hasObj());

        // Get all the fields with that name from the document.
        BSONElementSet geom;
        member->obj.getFieldsDotted(_params.nearQuery.field, geom, false);
        if (geom.empty()) {return PlanStage::NEED_TIME; }

        // Some value that any distance we can calculate will be less than.
        double minDistance = numeric_limits<double>::max();
        BSONObj minDistanceObj;
        for (BSONElementSet::iterator git = geom.begin(); git != geom.end(); ++git) {
            if (!git->isABSONObj()) { return PlanStage::FAILURE; }
            BSONObj obj = git->Obj();

            double distToObj;
            if (S2SearchUtil::distanceBetween(_params.nearQuery.centroid.point, obj, &distToObj)) {
                if (distToObj < minDistance) {
                    minDistance = distToObj;
                    minDistanceObj = obj;
                }
            }
            else {
                warning() << "unknown geometry: " << obj.toString();
            }
        }

        // If the distance to the doc satisfies our distance criteria, add it to our buffered
        // results.
        if (minDistance >= _innerRadius &&
            (_outerRadiusInclusive ? minDistance <= _outerRadius : minDistance < _outerRadius)) {
            _results.push(Result(*out, minDistance));
            if (_params.addDistMeta) {
                member->addComputed(new GeoDistanceComputedData(minDistance));
            }
            if (_params.addPointMeta) {
                member->addComputed(new GeoNearPointComputedData(minDistanceObj));
            }
            if (member->hasLoc()) {
                _invalidationMap[member->loc] = *out;
            }
        }

        return PlanStage::NEED_TIME;
    }

    void S2NearStage::prepareToYield() {
        if (NULL != _child.get()) {
            _child->prepareToYield();
        }
    }

    void S2NearStage::recoverFromYield() {
        if (NULL != _child.get()) {
            _child->recoverFromYield();
        }
    }

    void S2NearStage::invalidate(const DiskLoc& dl, InvalidationType type) {
        if (NULL != _child.get()) {
            _child->invalidate(dl, type);
        }

        // _results is a queue of results that we will return for the current shell we're on.
        // If a result is in _results and has a DiskLoc it will be in _invalidationMap as well.
        // It's safe to return the result w/o the DiskLoc.
        unordered_map<DiskLoc, WorkingSetID, DiskLoc::Hasher>::iterator it
            = _invalidationMap.find(dl);

        if (it != _invalidationMap.end()) {
            WorkingSetMember* member = _ws->get(it->second);
            verify(member->hasLoc());
            WorkingSetCommon::fetchAndInvalidateLoc(member);
            verify(!member->hasLoc());
            // Don't keep it around in the invalidation map since there's no valid DiskLoc anymore.
            _invalidationMap.erase(it);
        }
    }

    bool S2NearStage::isEOF() {
        if (!_worked) { return false; }
        if (_failed) { return true; }
        // We're only done if we exhaust the search space.
        return _innerRadius >= _maxDistance;
    }

    PlanStageStats* S2NearStage::getStats() {
        // TODO: must agg stats across child ixscan/fetches.
        // TODO: we can do better than this, need own common stats.
        _commonStats.isEOF = isEOF();
        return new PlanStageStats(_commonStats, STAGE_GEO_NEAR_2DSPHERE);
    }

}  // namespace mongo
