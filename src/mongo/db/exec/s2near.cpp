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

#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/geo/geoconstants.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/index/expression_index.h"
#include "third_party/s2/s2regionintersection.h"

namespace mongo {

    S2NearStage::S2NearStage(const string& ns, const BSONObj& indexKeyPattern,
                             const NearQuery& nearQuery, const IndexBounds& baseBounds,
                             MatchExpression* filter, WorkingSet* ws) {
        _ns = ns;
        _ws = ws;
        _indexKeyPattern = indexKeyPattern;
        _nearQuery = nearQuery;
        _baseBounds = baseBounds;
        _filter = filter;
        _worked = false;
        _failed = false;

        // The field we're near-ing from is the n-th field.  Figure out what that 'n' is.  We
        // put the cover for the search annulus in this spot in the bounds.
        _nearFieldIndex = 0;
        BSONObjIterator specIt(_indexKeyPattern);
        while (specIt.more()) {
             if (specIt.next().fieldName() == _nearQuery.field) {
                 break;
             }
            ++_nearFieldIndex;
        }

        verify(_nearFieldIndex < _indexKeyPattern.nFields());

        // FLAT implies the distances are in radians.  Convert to meters.
        if (FLAT == _nearQuery.centroid.crs) {
            _nearQuery.minDistance *= kRadiusOfEarthInMeters;
            _nearQuery.maxDistance *= kRadiusOfEarthInMeters;
        }

        // Make sure distances are sane.  Possibly redundant given the checking during parsing.
        _minDistance = max(0.0, _nearQuery.minDistance);
        _maxDistance = min(M_PI * kRadiusOfEarthInMeters, _nearQuery.maxDistance);
        _minDistance = min(_minDistance, _maxDistance);

        // We grow _outerRadius in nextAnnulus() below.
        _innerRadius = _outerRadius = _minDistance;

        // XXX: where do we grab finestIndexedLevel from really?  idx descriptor?
        int finestIndexedLevel = S2::kAvgEdge.GetClosestLevel(500.0 / kRadiusOfEarthInMeters);
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
        if (_failed) { return PlanStage::FAILURE; }
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
                unordered_map<DiskLoc, WorkingSetID, DiskLoc::Hasher>::iterator it = _invalidationMap.find(member->loc);
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
        _outerRadius = min(_outerRadius, _maxDistance);
        verify(_innerRadius <= _outerRadius);

        // We might have just grown our radius beyond anything reasonable.
        if (isEOF()) { return; }

        // Step 2: Fill out bounds for the ixscan we use.
        _innerCap = S2Cap::FromAxisAngle(_nearQuery.centroid.point,
                                         S1Angle::Radians(_innerRadius / kRadiusOfEarthInMeters));
        _outerCap = S2Cap::FromAxisAngle(_nearQuery.centroid.point,
                                         S1Angle::Radians(_outerRadius / kRadiusOfEarthInMeters));
        _innerCap = _innerCap.Complement();

        vector<S2Region*> regions;
        regions.push_back(&_innerCap);
        regions.push_back(&_outerCap);

        _annulus.Release(NULL);
        _annulus.Init(&regions);

        _baseBounds.fields[_nearFieldIndex].intervals.clear();
        ExpressionMapping::cover2dsphere(_annulus, &_baseBounds.fields[_nearFieldIndex]);

        // Step 3: Actually create the ixscan.
        // TODO: Cache params.
        IndexScanParams params;
        NamespaceDetails* nsd = nsdetails(_ns.c_str());
        if (NULL == nsd) {
            _failed = true;
            return;
        }
        int idxNo = nsd->findIndexByKeyPattern(_indexKeyPattern);
        if (-1 == idxNo) {
            _failed = true;
            return;
        }
        params.descriptor = CatalogHack::getDescriptor(nsd, idxNo);
        params.bounds = _baseBounds;
        params.direction = 1;
        IndexScan* scan = new IndexScan(params, _ws, NULL);

        // Owns 'scan'.
        _child.reset(new FetchStage(_ws, scan, _filter));
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
        member->obj.getFieldsDotted(_nearQuery.field, geom, false);
        if (geom.empty()) {return PlanStage::NEED_TIME; }

        // Some value that any distance we can calculate will be less than.
        double minDistance = numeric_limits<double>::max();
        for (BSONElementSet::iterator git = geom.begin(); git != geom.end(); ++git) {
            if (!git->isABSONObj()) { return PlanStage::FAILURE; }
            BSONObj obj = git->Obj();

            double distToObj;
            if (S2SearchUtil::distanceBetween(_nearQuery.centroid.point, obj, &distToObj)) {
                minDistance = min(distToObj, minDistance);
            }
            else {
                warning() << "unknown geometry: " << obj.toString();
            }
        }

        // If the distance to the doc satisfies our distance criteria,
        if (minDistance >= _innerRadius && minDistance < _outerRadius) {
            _results.push(Result(*out, minDistance));
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

    void S2NearStage::invalidate(const DiskLoc& dl) {
        if (NULL != _child.get()) {
            _child->invalidate(dl);
        }

        unordered_map<DiskLoc, WorkingSetID, DiskLoc::Hasher>::iterator it
            = _invalidationMap.find(dl);

        if (it != _invalidationMap.end()) {
            WorkingSetMember* member = _ws->get(it->second);
            verify(member->hasLoc());
            WorkingSetCommon::fetchAndInvalidateLoc(member);
            verify(!member->hasLoc());
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
