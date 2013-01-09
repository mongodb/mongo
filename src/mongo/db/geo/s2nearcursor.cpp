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
                       const BSONObj &query, const vector<QueryGeometry> &fields,
                       const S2IndexingParams &params, int numWanted, double maxDistance)
        : _details(details), _fields(fields), _params(params), _nscanned(0),
          _keyPattern(keyPattern), _numToReturn(numWanted),
          // _outerRadius can't be greater than (pi * r) or we wrap around the opposite
          // side of the world.
          _maxDistance(min(M_PI * _params.radius, maxDistance)) {
        BSONObjBuilder geoFieldsToNuke;
        for (size_t i = 0; i < _fields.size(); ++i) {
            geoFieldsToNuke.append(_fields[i].field, "");
        }
        // false means we want to filter OUT geoFieldsToNuke, not filter to include only that.
        _filteredQuery = query.filterFieldsUndotted(geoFieldsToNuke.obj(), false);
        // We match on the whole query, since it might have $within.
        _matcher.reset(new CoveredIndexMatcher(query, keyPattern));

        // More indexing machinery.
        BSONObjBuilder specBuilder;
        BSONObjIterator specIt(_keyPattern);
        while (specIt.more()) {
            BSONElement e = specIt.next();
            specBuilder.append(e.fieldName(), 1);
        }
        BSONObj spec = specBuilder.obj();
        _specForFRV = IndexSpec(spec);

        // Start with a conservative _radiusIncrement.
        _radiusIncrement = S2::kAvgEdge.GetValue(_params.finestIndexedLevel) * _params.radius;
        _innerRadius = _outerRadius = 0;
        // Set up _outerRadius with proper checks (maybe maxDistance is really small?)
        nextAnnulus();
    }

    S2NearCursor::~S2NearCursor() { }

    CoveredIndexMatcher* S2NearCursor::matcher() const { return _matcher.get(); }

    Record* S2NearCursor::_current() { return _results.top().loc.rec(); }
    BSONObj S2NearCursor::current() { return _results.top().loc.obj(); }
    DiskLoc S2NearCursor::currLoc() { return _results.top().loc; }
    BSONObj S2NearCursor::currKey() const { return _results.top().key; }
    DiskLoc S2NearCursor::refLoc() { return DiskLoc(); }
    long long S2NearCursor::nscanned() { return _nscanned; }

    double S2NearCursor::currentDistance() const { return _results.top().distance; }

    // TODO: yielding is very un-tested.
    // This is called when we're about to yield.
    void S2NearCursor::noteLocation() { _results = priority_queue<Result>(); }
    // Called when we're un-yielding.
    // Note that this is (apparently) a valid call sequence:
    // 1. noteLocation()
    // 2. ok()
    // 3. checkLocation()
    // As such we might have results and only want to fill the result queue if it's empty.
    void S2NearCursor::checkLocation() { if (_results.empty()) { fillResults(); } }

    void S2NearCursor::explainDetails(BSONObjBuilder& b) {
        // TODO(hk): Dump more meaningful stats.
        b << "nscanned " << _nscanned;
    }

    bool S2NearCursor::ok() {
        if (_numToReturn <= 0) { return false; }
        if (_innerRadius > _maxDistance) { return false; }
        if (_results.empty()) { fillResults(); }
        // If fillResults can't find anything, we're outta results.
        return !_results.empty();
    }

    void S2NearCursor::nextAnnulus() {
        _innerRadius = _outerRadius;
        _outerRadius += _radiusIncrement;
        _outerRadius = min(_outerRadius, _maxDistance);
        verify(_innerRadius <= _outerRadius);
    }

    bool S2NearCursor::advance() {
        if (_numToReturn <= 0) { return false; }
        if (_innerRadius > _maxDistance) { return false; }

        if (!_results.empty()) {
            _returned.insert(_results.top().loc);
            _results.pop();
            --_numToReturn;
            ++_nscanned;
            // Safe to grow the radius as we've returned everything in our shell.  We don't do this
            // check outside of !_results.empty() because we could have results, yield, dump them
            // (_results would be empty), then need to recreate them w/the same radii.  In that case
            // we'd grow when we shouldn't.
            if (_results.empty()) { nextAnnulus(); }
        }

        if (_results.empty()) { fillResults(); }

        // The only reason this should be empty is if there are no more results to be had.
        return !_results.empty();
    }

    BSONObj S2NearCursor::makeFRSObject() {
        BSONObjBuilder frsObjBuilder;
        frsObjBuilder.appendElements(_filteredQuery);

        S2RegionCoverer coverer;
        // Step 1: Make the monstrous BSONObj that describes what keys we want.
        for (size_t i = 0; i < _fields.size(); ++i) {
            const QueryGeometry &field = _fields[i];
            S2Point center = field.getCentroid();
            BSONObj inExpr;
            // Caps are inclusive and inverting a cap includes the border.  This means that our
            // initial _innerRadius of 0 is OK -- we'll still find a point that is exactly at
            // the start of our search.
            S2Cap innerCap = S2Cap::FromAxisAngle(center, S1Angle::Radians(_innerRadius / _params.radius));
            S2Cap invInnerCap = innerCap.Complement();
            S2Cap outerCap = S2Cap::FromAxisAngle(center, S1Angle::Radians(_outerRadius / _params.radius));
            vector<S2Region*> regions;
            regions.push_back(&invInnerCap);
            regions.push_back(&outerCap);
            S2RegionIntersection shell(&regions);
            vector<S2CellId> cover;
            _params.configureCoverer(&coverer);
            coverer.GetCovering(shell, &cover);
            // TODO(hk): Remove this.  Grow the covering size when we grow the radius.
            if (cover.size() > 5000) {
                int oldCoverSize = cover.size();
                int oldMaxLevel = coverer.max_level();
                coverer.set_max_level(coverer.min_level());
                coverer.set_min_level(3 * coverer.min_level() / 4);
                coverer.GetCovering(shell, &cover);
                LOG(2) << "Trying to create BSON indexing obj w/too many regions = " << oldCoverSize
                    << endl;
                LOG(2) << "Modifying coverer from (" << coverer.max_level() << "," << oldMaxLevel
                    << ") to (" << coverer.min_level() << "," << coverer.max_level() << ")"
                    << endl;
                LOG(2) << "New #regions = " << cover.size() << endl;
            }
            inExpr = S2SearchUtil::coverAsBSON(cover, field.field, _params.coarsestIndexedLevel);
            // Shell takes ownership of the regions we push in, but they're local variables and
            // deleting them would be bad.
            shell.Release(NULL);
            frsObjBuilder.appendElements(inExpr);
        }
        return frsObjBuilder.obj();
    }

    // Fill _results with the next shell of results.  We may have to search several times to do
    // this.  If _results.empty() after calling fillResults, there are no more possible results.
    void S2NearCursor::fillResults() {
        verify(_results.empty());
        if (_innerRadius >= _outerRadius) { return; }
        if (_innerRadius > _maxDistance) { return; }
        if (0 == _numToReturn) { return; }

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
            set<DiskLoc> seen;

            // Do the actual search through this annulus.
            size_t considered = 0;
            for (; cursor->ok(); cursor->advance()) {
                ++considered;

                if (seen.end() != seen.find(cursor->currLoc())) { continue; }
                seen.insert(cursor->currLoc());

                MatchDetails details;
                bool matched = _matcher->matchesCurrent(cursor.get(), &details);
                if (!matched) { continue; }

                const BSONObj& indexedObj = cursor->currLoc().obj();

                size_t geoFieldsInRange = 0;
                double minMatchingDistance = 1e20;

                // Calculate the distance from our query point(s) to the geo field(s).
                // For each geo field in the query...
                for (size_t i = 0; i < _fields.size(); ++i) {
                    const QueryGeometry& field = _fields[i];

                    // Get all the fields with that name from the document.
                    BSONElementSet geoFieldElements;
                    indexedObj.getFieldsDotted(field.field, geoFieldElements, false);
                    if (geoFieldElements.empty()) { continue; }

                    // For each field with that name in the document...
                    for (BSONElementSet::iterator oi = geoFieldElements.begin();
                            oi != geoFieldElements.end(); ++oi) {
                        if (!oi->isABSONObj()) { continue; }
                        double dist = distanceBetween(field, oi->Obj());
                        // If it satisfies our distance criteria...
                        if (dist >= _innerRadius && dist <= _outerRadius) {
                            // Success!  For this field.
                            ++geoFieldsInRange;
                            minMatchingDistance = min(dist, minMatchingDistance);
                        }
                    }
                }
                // If all the geo query fields had something in range
                if (_fields.size() == geoFieldsInRange) {
                    // The result is valid.  We have to de-dup ourselves here.
                    if (_returned.end() == _returned.find(cursor->currLoc())) {
                        _results.push(Result(cursor->currLoc(), cursor->currKey(),
                                             minMatchingDistance));
                    }
                }
            }
            if (_results.empty()) {
                _radiusIncrement *= 2;
                nextAnnulus();
            }
        } while (_results.empty()
                 && _innerRadius < _maxDistance
                 && _innerRadius < _outerRadius);
        // TODO: consider shrinking _radiusIncrement if _results.size() meets some criteria.
    }

    double S2NearCursor::distanceBetween(const QueryGeometry &field, const BSONObj &obj) {
        S2Point us = field.getCentroid();
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
        }
        S1Angle angle(us, them);
        return angle.radians() * _params.radius;
    }
}  // namespace mongo
