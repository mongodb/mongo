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

#include "mongo/db/btree.h"
#include "mongo/db/index.h"
#include "mongo/db/matcher.h"
#include "mongo/db/pdfile.h"
#include "third_party/s2/s2cap.h"
#include "third_party/s2/s2regionintersection.h"
#include "mongo/db/geo/s2common.h"
#include "mongo/db/geo/s2nearcursor.h"

namespace mongo {
    S2NearCursor::S2NearCursor(const BSONObj &keyPattern, const IndexDetails *details,
                       const BSONObj &query, const NearQuery &nearQuery,
                       const vector<GeoQuery> &indexedGeoFields,
                       const S2IndexingParams &params)
        : _details(details), _nearQuery(nearQuery), _indexedGeoFields(indexedGeoFields),
          _params(params), _keyPattern(keyPattern), _nearFieldIndex(0), _returnedDistance(0) {

        BSONObjBuilder geoFieldsToNuke;
        for (size_t i = 0; i < _indexedGeoFields.size(); ++i) {
            geoFieldsToNuke.append(_indexedGeoFields[i].getField(), "");
        }
        // false means we want to filter OUT geoFieldsToNuke, not filter to include only that.
        _filteredQuery = query.filterFieldsUndotted(geoFieldsToNuke.obj(), false);
        _matcher.reset(new CoveredIndexMatcher(_filteredQuery, keyPattern));

        // More indexing machinery.
        BSONObjBuilder specBuilder;
        BSONObjIterator specIt(_keyPattern);
        while (specIt.more()) {
            BSONElement e = specIt.next();
            specBuilder.append(e.fieldName(), 1);
        }
        BSONObj spec = specBuilder.obj();
        _specForFRV = IndexSpec(spec);

        specIt = BSONObjIterator(_keyPattern);
        while (specIt.more()) {
            if (specIt.next().fieldName() == _nearQuery.field) { break; }
            ++_nearFieldIndex;
        }

        // _outerRadius can't be greater than (pi * r) or we wrap around the opposite
        // side of the world.
        _maxDistance = min(M_PI * _params.radius, _nearQuery.maxDistance);

        // Start with a conservative _radiusIncrement.
        _radiusIncrement = 5 * S2::kAvgEdge.GetValue(_params.finestIndexedLevel) * _params.radius;
        _innerRadius = _outerRadius = 0;
        // We might want to adjust the sizes of our coverings if our search
        // isn't local to the start point.
        // Set up _outerRadius with proper checks (maybe maxDistance is really small?)
        nextAnnulus();
    }

    S2NearCursor::~S2NearCursor() {
        // Annulus takes ownership of the pointers we pass in.
        // Those are actually pointers to the member variables _innerCap and _outerCap.
        _annulus.Release(NULL);
    }

    CoveredIndexMatcher* S2NearCursor::matcher() const { return _matcher.get(); }

    Record* S2NearCursor::_current() { return _results.top().loc.rec(); }
    BSONObj S2NearCursor::current() { return _results.top().loc.obj(); }
    DiskLoc S2NearCursor::currLoc() { return _results.top().loc; }
    BSONObj S2NearCursor::currKey() const { return _results.top().key; }
    DiskLoc S2NearCursor::refLoc() { return DiskLoc(); }
    long long S2NearCursor::nscanned() { return _stats._nscanned; }

    double S2NearCursor::currentDistance() const { return _results.top().distance; }

    // This is called when we're about to yield.
    void S2NearCursor::noteLocation() {
        LOG(1) << "yielding, tossing " << _results.size() << " results" << endl;
        _results = priority_queue<Result>();
    }

    // Called when we're un-yielding.
    // Note that this is (apparently) a valid call sequence:
    // 1. noteLocation()
    // 2. ok()
    // 3. checkLocation()
    // As such we might have results and only want to fill the result queue if it's empty.
    void S2NearCursor::checkLocation() {
        LOG(1) << "unyielding, have " << _results.size() << " results in queue";
        if (_results.empty()) {
            LOG(1) << ", filling..." << endl;
            fillResults();
            LOG(1) << "now have " << _results.size() << " results in queue";
        }
        LOG(1) << endl;
    }

    void S2NearCursor::explainDetails(BSONObjBuilder& b) {
        b << "nscanned" << _stats._nscanned;
        b << "matchTested" << _stats._matchTested;
        b << "geoMatchTested" << _stats._geoMatchTested;
        b << "numShells" << _stats._numShells;
        b << "keyGeoSkip" << _stats._keyGeoSkip;
        b << "returnSkip" << _stats._returnSkip;
        b << "btreeDups" << _stats._btreeDups;
        b << "inAnnulusTested" << _stats._inAnnulusTested;
    }

    bool S2NearCursor::ok() {
        if (_innerRadius > _maxDistance) {
            LOG(1) << "not OK, exhausted search bounds" << endl;
            return false;
        }
        if (_results.empty()) {
            LOG(1) << "results empty in OK, filling" << endl;
            fillResults();
        }
        // If fillResults can't find anything, we're outta results.
        return !_results.empty();
    }

    void S2NearCursor::nextAnnulus() {
        LOG(1) << "growing annulus from (" << _innerRadius << ", " << _outerRadius;
        _innerRadius = _outerRadius;
        _outerRadius += _radiusIncrement;
        _outerRadius = min(_outerRadius, _maxDistance);
        verify(_innerRadius <= _outerRadius);
        LOG(1) << ") to (" << _innerRadius << ", " << _outerRadius << ")" << endl;
        ++_stats._numShells;
    }

    bool S2NearCursor::advance() {
        if (_innerRadius > _maxDistance) {
            LOG(2) << "advancing but exhausted search distance" << endl;
            return false;
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

        // The only reason _results should be empty now is if there are no more possible results.
        return !_results.empty();
    }

    BSONObj S2NearCursor::makeFRSObject() {
        BSONObjBuilder frsObjBuilder;
        frsObjBuilder.appendElements(_filteredQuery);

        S2RegionCoverer coverer;
        // Step 1: Make the BSON'd covering for our search annulus.
        BSONObj inExpr;
        // Caps are inclusive and inverting a cap includes the border.  This means that our
        // initial _innerRadius of 0 is OK -- we'll still find a point that is exactly at
        // the start of our search.
        _innerCap = S2Cap::FromAxisAngle(_nearQuery.centroid,
                                         S1Angle::Radians(_innerRadius / _params.radius));
        _outerCap = S2Cap::FromAxisAngle(_nearQuery.centroid,
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
            uassert(16682, "Couldn't generate index keys for geo field "
                       + _indexedGeoFields[i].getField(),
                    cover.size() > 0);
            BSONObj fieldRange = S2SearchUtil::coverAsBSON(cover, _indexedGeoFields[i].getField(),
                _params.coarsestIndexedLevel);
            frsObjBuilder.appendElements(fieldRange);
        }

        return frsObjBuilder.obj();
    }

    // Fill _results with the next shell of results.  We may have to search several times to do
    // this.  If _results.empty() after calling fillResults, there are no more possible results.
    void S2NearCursor::fillResults() {
        verify(_results.empty());
        if (_innerRadius >= _outerRadius) { return; }
        if (_innerRadius > _maxDistance) { return; }

        // We iterate until 1. our search radius is too big or 2. we find results.
        do {
            // Some of these arguments are opaque, look at the definitions of the involved classes.
            FieldRangeSet frs(_details->parentNS().c_str(), makeFRSObject(), false, false);
            shared_ptr<FieldRangeVector> frv(new FieldRangeVector(frs, _specForFRV, 1));
            scoped_ptr<BtreeCursor> cursor(BtreeCursor::make(nsdetails(_details->parentNS()),
                                                             *_details, frv, 0, 1));

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

                seen.insert(cursor->currLoc());

                // Get distance interval from our query point to the cell.
                // If it doesn't overlap with our current shell, toss.
                BSONObjIterator it(cursor->currKey());
                BSONElement geoKey;
                for (int i = 0; i <= _nearFieldIndex; ++i) {
                    geoKey = it.next();
                }

                S2Cell keyCell = S2Cell(S2CellId::FromString(geoKey.String()));
                if (!_annulus.MayIntersect(keyCell)) {
                    ++_stats._keyGeoSkip;
                    continue;
                }

                // Match against non-indexed fields.
                ++_stats._matchTested;
                MatchDetails details;
                if (!_matcher->matchesCurrent(cursor.get(), &details)) {
                    continue;
                }

                const BSONObj& indexedObj = cursor->currLoc().obj();

                // Match against indexed geo fields.
                ++_stats._geoMatchTested;
                size_t geoFieldsMatched = 0;
                // OK, cool, non-geo match satisfied.  See if the object actually overlaps w/the geo
                // query fields.
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
                        uassert(16699, "ill-formed geometry: " + geoObj.toString(),
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
                    double dist = distanceTo(oi->Obj());
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

    double S2NearCursor::distanceTo(const BSONObj &obj) {
        const S2Point &us = _nearQuery.centroid;
        S2Point them;

        S2Polygon polygon;
        S2Polyline line;
        S2Cell point;
        if (GeoParser::parsePolygon(obj, &polygon)) {
            them = polygon.Project(us);
        } else if (GeoParser::parseLineString(obj, &line)) {
            int tmp;
            them = line.Project(us, &tmp);
        } else if (GeoParser::parsePoint(obj, &point)) {
            them = point.GetCenter();
        } else {
            warning() << "unknown geometry: " << obj.toString();
            return numeric_limits<double>::max();
        }
        S1Angle angle(us, them);
        return angle.radians() * _params.radius;
    }
}  // namespace mongo
