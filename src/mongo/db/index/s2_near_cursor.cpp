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

#include "mongo/db/index/s2_near_cursor.h"

#include "mongo/db/btreecursor.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/queryutil.h"
#include "third_party/s2/s2regionintersection.h"

namespace mongo {

    S2NearIndexCursor::S2NearIndexCursor(IndexDescriptor* descriptor,
                                         const S2IndexingParams& params)
        : _descriptor(descriptor), _params(params) { }

    void S2NearIndexCursor::seek(const BSONObj& query, const NearQuery& nearQuery,
                                 const vector<GeoQuery>& regions) {
        _indexedGeoFields = regions;
        _nearQuery = nearQuery;
        _returnedDistance = 0;
        _nearFieldIndex = 0;
        _stats = Stats();
        _returned = unordered_set<DiskLoc, DiskLoc::Hasher>();
        _results = priority_queue<Result>();

        BSONObjBuilder geoFieldsToNuke;
        for (size_t i = 0; i < _indexedGeoFields.size(); ++i) {
            geoFieldsToNuke.append(_indexedGeoFields[i].getField(), "");
        }
        // false means we want to filter OUT geoFieldsToNuke, not filter to include only that.
        _filteredQuery = query.filterFieldsUndotted(geoFieldsToNuke.obj(), false);

        // More indexing machinery.
        BSONObjBuilder specBuilder;
        BSONObjIterator specIt(_descriptor->keyPattern());
        while (specIt.more()) {
            BSONElement e = specIt.next();
            // Checked in AccessMethod already, so we know this spec has only numbers and 2dsphere
            if ( e.type() == String ) {
                specBuilder.append( e.fieldName(), 1 );
            }
            else {
                specBuilder.append( e.fieldName(), e.numberInt() );
            }
        }
        _specForFRV = specBuilder.obj();

        specIt = BSONObjIterator(_descriptor->keyPattern());
        while (specIt.more()) {
            if (specIt.next().fieldName() == _nearQuery.field) { break; }
            ++_nearFieldIndex;
        }

        _minDistance = max(0.0, _nearQuery.minDistance);
        
        // _outerRadius can't be greater than (pi * r) or we wrap around the opposite
        // side of the world.
        _maxDistance = min(M_PI * _params.radius, _nearQuery.maxDistance);
        uassert(16892, "$minDistance too large", _minDistance < _maxDistance);

        // Start with a conservative _radiusIncrement.
        _radiusIncrement = 5 * S2::kAvgEdge.GetValue(_params.finestIndexedLevel) * _params.radius;
        _innerRadius = _outerRadius = _minDistance;
        // We might want to adjust the sizes of our coverings if our search
        // isn't local to the start point.
        // Set up _outerRadius with proper checks (maybe maxDistance is really small?)
        nextAnnulus();
        fillResults();
    }

    S2NearIndexCursor::~S2NearIndexCursor() { _annulus.Release(NULL); }

    Status S2NearIndexCursor::seek(const BSONObj& position) {
        return Status::OK();
    }

    Status S2NearIndexCursor::setOptions(const CursorOptions& options) {
        return Status::OK();
    }

    bool S2NearIndexCursor::isEOF() const {
        if (_innerRadius > _maxDistance) {
            return true;
        }

        return _results.empty();
    }

    BSONObj S2NearIndexCursor::getKey() const { return _results.top().key; }
    DiskLoc S2NearIndexCursor::getValue() const { return _results.top().loc; }
    string S2NearIndexCursor::toString() { return "S2NearCursor"; }

    void S2NearIndexCursor::next() {
        if (_innerRadius > _maxDistance) {
            LOG(2) << "advancing but exhausted search distance" << endl;
        }

        if (!_results.empty()) {
            _returnedDistance = _results.top().distance;
            _returned.insert(_results.top().loc);
            _results.pop();
            ++_stats._numReturned;
            // Safe to grow the radius as we've returned everything in our shell.  We don't do this
            // check outside of !_results.empty() because we could have results, yield, dump them
            // (_results would be empty), then need to recreate them w/the same radii.  In that case
            // we'd grow when we shouldn't.
            if (_results.empty()) { nextAnnulus(); }
        }

        if (_results.empty()) { fillResults(); }
    }

    Status S2NearIndexCursor::savePosition() {
        _results = priority_queue<Result>();
        return Status::OK();
    }

    Status S2NearIndexCursor::restorePosition() {
        if (_results.empty()) {
            fillResults();
        }
        return Status::OK();
    }

    // Make the object that describes all keys that are within our current search annulus.
    BSONObj S2NearIndexCursor::makeFRSObject() {
        BSONObjBuilder frsObjBuilder;
        frsObjBuilder.appendElements(_filteredQuery);

        S2RegionCoverer coverer;
        // Step 1: Make the BSON'd covering for our search annulus.
        BSONObj inExpr;
        // Caps are inclusive and inverting a cap includes the border.  This means that our
        // initial _innerRadius of 0 is OK -- we'll still find a point that is exactly at
        // the start of our search.
        _innerCap = S2Cap::FromAxisAngle(_nearQuery.centroid.point,
                S1Angle::Radians(_innerRadius / _params.radius));
        _outerCap = S2Cap::FromAxisAngle(_nearQuery.centroid.point,
                S1Angle::Radians(_outerRadius / _params.radius));
        double area = _outerCap.area() - _innerCap.area();
        _innerCap = _innerCap.Complement();
        vector<S2Region*> regions;
        regions.push_back(&_innerCap);
        regions.push_back(&_outerCap);
        _annulus.Release(NULL);
        _annulus.Init(&regions);
        vector<S2CellId> cover;
        S2SearchUtil::setCoverLimitsBasedOnArea(area, &coverer, _params.coarsestIndexedLevel);
        coverer.GetCovering(_annulus, &cover);
        LOG(2) << "annulus cover size is " << cover.size()
            << ", params (" << coverer.min_level() << ", " << coverer.max_level() << ")"
            << endl;
        inExpr = S2SearchUtil::coverAsBSON(cover, _nearQuery.field,
                _params.coarsestIndexedLevel);
        frsObjBuilder.appendElements(inExpr);

        _params.configureCoverer(&coverer);
        // Cover the indexed geo components of the query.
        for (size_t i = 0; i < _indexedGeoFields.size(); ++i) {
            vector<S2CellId> cover;
            coverer.GetCovering(_indexedGeoFields[i].getRegion(), &cover);
            uassert(16761, "Couldn't generate index keys for geo field "
                    + _indexedGeoFields[i].getField(),
                    cover.size() > 0);
            BSONObj fieldRange = S2SearchUtil::coverAsBSON(cover, _indexedGeoFields[i].getField(),
                    _params.coarsestIndexedLevel);
            frsObjBuilder.appendElements(fieldRange);
        }

        return frsObjBuilder.obj();
    }

    // Fill _results with all of the results in the annulus defined by _innerRadius and
    // _outerRadius.  If no results are found, grow the annulus and repeat until success (or
    // until the edge of the world).
    void S2NearIndexCursor::fillResults() {
        verify(_results.empty());
        if (_innerRadius >= _outerRadius) { return; }
        if (_innerRadius > _maxDistance) { return; }

        // We iterate until 1. our search radius is too big or 2. we find results.
        do {
            // Some of these arguments are opaque, look at the definitions of the involved classes.
            FieldRangeSet frs(_descriptor->parentNS().c_str(), makeFRSObject(), false, false);
            shared_ptr<FieldRangeVector> frv(new FieldRangeVector(frs, _specForFRV, 1));
            scoped_ptr<BtreeCursor> cursor(BtreeCursor::make(nsdetails(_descriptor->parentNS()),
                        _descriptor->getOnDisk(), frv, 0, 1));

            // The cursor may return the same obj more than once for a given
            // FRS, so we make sure to only consider it once in any given annulus.
            //
            // We don't want this outside of the 'do' loop because the covering
            // for an annulus may return an object whose distance to the query
            // point is actually contained in a subsequent annulus.  If we
            // didn't consider every object in a given annulus we might miss
            // the point.
            //
            // We don't use a global 'seen' because we get that by requiring
            // the distance from the query point to the indexed geo to be
            // within our 'current' annulus, and I want to dodge all yield
            // issues if possible.
            unordered_set<DiskLoc, DiskLoc::Hasher> seen;

            LOG(1) << "looking at annulus from " << _innerRadius << " to " << _outerRadius << endl;
            LOG(1) << "Total # returned: " << _stats._numReturned << endl;
            // Do the actual search through this annulus.
            for (; cursor->ok(); cursor->advance()) {
                // Don't bother to look at anything we've returned.
                if (_returned.end() != _returned.find(cursor->currLoc())) {
                    ++_stats._returnSkip;
                    continue;
                }

                ++_stats._nscanned;
                if (seen.end() != seen.find(cursor->currLoc())) {
                    ++_stats._btreeDups;
                    continue;
                }

                // Get distance interval from our query point to the cell.
                // If it doesn't overlap with our current shell, toss.
                BSONObj currKey(cursor->currKey());
                BSONObjIterator it(currKey);
                BSONElement geoKey;
                for (int i = 0; i <= _nearFieldIndex; ++i) {
                    geoKey = it.next();
                }

                S2Cell keyCell = S2Cell(S2CellId::FromString(geoKey.String()));
                if (!_annulus.MayIntersect(keyCell)) {
                    ++_stats._keyGeoSkip;
                    continue;
                }

                // We have to add this document to seen *AFTER* the key intersection test.
                // A geometry may have several keys, one of which may be in our search shell and one
                // of which may be outside of it.  We don't want to ignore a document just because
                // one of its covers isn't inside this annulus.
                seen.insert(cursor->currLoc());

                // At this point forward, we will not examine the document again in this annulus.

                const BSONObj& indexedObj = cursor->currLoc().obj();

                // Match against indexed geo fields.
                ++_stats._geoMatchTested;
                size_t geoFieldsMatched = 0;
                // See if the object actually overlaps w/the geo query fields.
                for (size_t i = 0; i < _indexedGeoFields.size(); ++i) {
                    BSONElementSet geoFieldElements;
                    indexedObj.getFieldsDotted(_indexedGeoFields[i].getField(), geoFieldElements,
                            false);
                    if (geoFieldElements.empty()) { continue; }

                    bool match = false;

                    for (BSONElementSet::iterator oi = geoFieldElements.begin();
                            !match && (oi != geoFieldElements.end()); ++oi) {
                        if (!oi->isABSONObj()) { continue; }
                        const BSONObj &geoObj = oi->Obj();
                        GeometryContainer geoContainer;
                        uassert(16762, "ill-formed geometry: " + geoObj.toString(),
                                geoContainer.parseFrom(geoObj));
                        match = _indexedGeoFields[i].satisfiesPredicate(geoContainer);
                    }

                    if (match) { ++geoFieldsMatched; }
                }

                if (geoFieldsMatched != _indexedGeoFields.size()) {
                    continue;
                }

                // Get all the fields with that name from the document.
                BSONElementSet geoFieldElements;
                indexedObj.getFieldsDotted(_nearQuery.field, geoFieldElements, false);
                if (geoFieldElements.empty()) { continue; }

                ++_stats._inAnnulusTested;
                double minDistance = 1e20;
                // Look at each field in the document and take the min. distance.
                for (BSONElementSet::iterator oi = geoFieldElements.begin();
                        oi != geoFieldElements.end(); ++oi) {
                    if (!oi->isABSONObj()) { continue; }
                    BSONObj obj = oi->Obj();
                    double dist;
                    bool ret = S2SearchUtil::distanceBetween(_nearQuery.centroid.point,
                                                             obj, _params, &dist);
                    if (!ret) {
                        warning() << "unknown geometry: " << obj.toString();
                        dist = numeric_limits<double>::max();
                    }

                    minDistance = min(dist, minDistance);
                }

                // We could be in an annulus, yield, add new points closer to
                // query point than the last point we returned, then unyield.
                // This would return points out of order.
                if (minDistance < _returnedDistance) { continue; }

                // If the min. distance satisfies our distance criteria
                if (minDistance >= _innerRadius && minDistance < _outerRadius) {
                    // The result is valid.  We have to de-dup ourselves here.
                    if (_returned.end() == _returned.find(cursor->currLoc())) {
                        _results.push(Result(cursor->currLoc(), cursor->currKey(),
                                    minDistance));
                    }
                }
            }

            if (_results.empty()) {
                LOG(1) << "results empty!\n";
                _radiusIncrement *= 2;
                nextAnnulus();
            } else if (_results.size() < 300) {
                _radiusIncrement *= 2;
            } else if (_results.size() > 600) {
                _radiusIncrement /= 2;
            }
        } while (_results.empty()
                && _innerRadius < _maxDistance
                && _innerRadius < _outerRadius);
        LOG(1) << "Filled shell with " << _results.size() << " results" << endl;
    }

    // Grow _innerRadius and _outerRadius by _radiusIncrement, capping _outerRadius at halfway
    // around the world (pi * _params.radius).
    void S2NearIndexCursor::nextAnnulus() {
        LOG(1) << "growing annulus from (" << _innerRadius << ", " << _outerRadius;
        _innerRadius = _outerRadius;
        _outerRadius += _radiusIncrement;
        _outerRadius = min(_outerRadius, _maxDistance);
        verify(_innerRadius <= _outerRadius);
        LOG(1) << ") to (" << _innerRadius << ", " << _outerRadius << ")" << endl;
        ++_stats._numShells;
    }

}  // namespace mongo
