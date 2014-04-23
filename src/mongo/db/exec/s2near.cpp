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

        // FLAT implies the input distances are in radians.  Convert to meters.
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
        if ( !_params.collection ) {
            _failed = true;
            return;
        }

        _descriptor =
            _params.collection->getIndexCatalog()->findIndexByKeyPattern(_params.indexKeyPattern);
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

    /**
     * A MatchExpression for seeing if an S2Cell-in-a-key is within an annulus.
     */
    class GeoS2KeyMatchExpression : public MatchExpression {
    public:
        /**
         * 'annulus' must outlive 'this'.
         */
        GeoS2KeyMatchExpression(S2RegionIntersection* annulus,
                                StringData nearFieldPath)
            : MatchExpression(INTERNAL_GEO_S2_KEYCHECK),
              _annulus(annulus) {

            _elementPath.init(nearFieldPath);
        }   

        virtual ~GeoS2KeyMatchExpression(){}

        virtual bool matches(const MatchableDocument* doc, MatchDetails* details = 0) const {
            MatchableDocument::IteratorHolder cursor(doc, &_elementPath);

            while (cursor->more()) {
                ElementIterator::Context e = cursor->next();
                if (matchesSingleElement(e.element())) {
                    return true;
                }
            }

            return false;
        }

        virtual bool matchesSingleElement(const BSONElement& e) const {
            // Something has gone terribly wrong if this doesn't hold.
            invariant(String == e.type());
            S2Cell keyCell = S2Cell(S2CellId::FromString(e.str()));
            return _annulus->MayIntersect(keyCell);
        }

        //
        // These won't be called.
        //

        virtual void debugString( StringBuilder& debug, int level = 0 ) const {
        }

        virtual bool equivalent( const MatchExpression* other ) const {
            return false;
        }

        virtual MatchExpression* shallowClone() const {
            return NULL;
        }

    private:
        // Not owned here.
        S2RegionIntersection* _annulus;

        ElementPath _elementPath;
    };

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
        // We use a filter on the key.  The filter rejects keys that don't intersect with the
        // annulus.  An object that is in the annulus might have a key that's not in it and a key
        // that's in it.  As such we can't just look at one key per object.
        //
        // This does force us to do our own deduping of results, though.
        params.doNotDedup = true;

        // Owns geo filter.
        _keyGeoFilter.reset(new GeoS2KeyMatchExpression(
            &_annulus, _params.baseBounds.fields[_nearFieldIndex].name));
        IndexScan* scan = new IndexScan(params, _ws, _keyGeoFilter.get());

        // Owns 'scan'.
        _child.reset(new FetchStage(_ws, scan, _params.filter, _params.collection));
        _seenInScan.clear();
    }

    PlanStage::StageState S2NearStage::addResultToQueue(WorkingSetID* out) {
        PlanStage::StageState state = _child->work(out);

        // All done reading from _child.
        if (PlanStage::IS_EOF == state) {
            _child.reset();
            _keyGeoFilter.reset();

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

        WorkingSetMember* member = _ws->get(*out);
        // Must have an object in order to get geometry out of it.
        verify(member->hasObj());

        // The scans we use don't dedup so we must dedup them ourselves.  We only put locs into here
        // if we know for sure whether or not we'll return them in this annulus.
        if (member->hasLoc()) {
            if (_seenInScan.end() != _seenInScan.find(member->loc)) {
                return PlanStage::NEED_TIME;
            }
        }

        // Get all the fields with that name from the document.
        BSONElementSet geom;
        member->obj.getFieldsDotted(_params.nearQuery.field, geom, false);
        if (geom.empty()) {
            return PlanStage::NEED_TIME;
        }

        // Some value that any distance we can calculate will be less than.
        double minDistance = numeric_limits<double>::max();
        BSONObj minDistanceObj;
        for (BSONElementSet::iterator git = geom.begin(); git != geom.end(); ++git) {
            if (!git->isABSONObj()) {
                mongoutils::str::stream ss;
                ss << "s2near stage read invalid geometry element " << *git << " from child";
                Status status(ErrorCodes::InternalError, ss);
                *out = WorkingSetCommon::allocateStatusMember( _ws, status);
                return PlanStage::FAILURE;
            }
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

        // If we're here we'll either include the doc in this annulus or reject it.  It's safe to
        // ignore it if it pops up again in this annulus.
        if (member->hasLoc()) {
            _seenInScan.insert(member->loc);
        }

        // If the distance to the doc satisfies our distance criteria, add it to our buffered
        // results.
        if (minDistance >= _innerRadius &&
            (_outerRadiusInclusive ? minDistance <= _outerRadius : minDistance < _outerRadius)) {
            _results.push(Result(*out, minDistance));
            if (_params.addDistMeta) {
                // FLAT implies the output distances are in radians.  Convert to meters.
                if (FLAT == _params.nearQuery.centroid.crs) {
                    member->addComputed(new GeoDistanceComputedData(minDistance
                                                                    / kRadiusOfEarthInMeters));
                }
                else {
                    member->addComputed(new GeoDistanceComputedData(minDistance));
                }
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
