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
          _params(params), _keyPattern(keyPattern),
          _nscanned(0), _matchTested(0), _geoTested(0), _numShells(0) {

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

        // _outerRadius can't be greater than (pi * r) or we wrap around the opposite
        // side of the world.
        _maxDistance = min(M_PI * _params.radius, _nearQuery.maxDistance);

        // Start with a conservative _radiusIncrement.
        _radiusIncrement = S2::kAvgEdge.GetValue(_params.finestIndexedLevel) * _params.radius;
        _innerRadius = _outerRadius = 0;
        // We might want to adjust the sizes of our coverings if our search
        // isn't local to the start point.
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
        b << "nscanned" << _nscanned;
        b << "matchTested" << _matchTested;
        b << "geoTested" << _geoTested;
        b << "numShells" << _numShells;
    }

    bool S2NearCursor::ok() {
        if (_innerRadius > _maxDistance) {
            LOG(2) << "not OK, exhausted search bounds" << endl;
            return false;
        }
        if (_results.empty()) {
            LOG(2) << "results empty in OK, filling" << endl;
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
        ++_numShells;
    }

    bool S2NearCursor::advance() {
        if (_innerRadius > _maxDistance) {
            LOG(2) << "advancing but exhausted search distance" << endl;
            return false;
        }

        if (!_results.empty()) {
            _returned.insert(_results.top().loc);
            _results.pop();
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
        S2Cap innerCap = S2Cap::FromAxisAngle(_nearQuery.centroid,
                                              S1Angle::Radians(_innerRadius / _params.radius));
        S2Cap invInnerCap = innerCap.Complement();
        S2Cap outerCap = S2Cap::FromAxisAngle(_nearQuery.centroid,
                                              S1Angle::Radians(_outerRadius / _params.radius));
        vector<S2Region*> regions;
        regions.push_back(&invInnerCap);
        regions.push_back(&outerCap);
        S2RegionIntersection shell(&regions);
        vector<S2CellId> cover;
        double area = outerCap.area() - innerCap.area();
        S2SearchUtil::setCoverLimitsBasedOnArea(area, &coverer, _params.coarsestIndexedLevel);
        coverer.GetCovering(shell, &cover);
        LOG(2) << "annulus cover size is " << cover.size()
               << ", params (" << coverer.min_level() << ", " << coverer.max_level() << ")"
               << endl;
        inExpr = S2SearchUtil::coverAsBSON(cover, _nearQuery.field,
                                           _params.coarsestIndexedLevel);
        // Shell takes ownership of the regions we push in, but they're local variables and
        // deleting them would be bad.
        shell.Release(NULL);
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
            set<DiskLoc> seen;

            LOG(1) << "looking at annulus from " << _innerRadius << " to " << _outerRadius << endl;
            // Do the actual search through this annulus.
            for (; cursor->ok(); cursor->advance()) {
                ++_nscanned;
                if (seen.end() != seen.find(cursor->currLoc())) { continue; }
                seen.insert(cursor->currLoc());

                // Match against non-indexed fields.
                ++_matchTested;
                MatchDetails details;
                bool matched = _matcher->matchesCurrent(cursor.get(), &details);
                if (!matched) { continue; }

                const BSONObj& indexedObj = cursor->currLoc().obj();

                ++_geoTested;
                // Match against indexed geo fields.
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

                if (geoFieldsMatched != _indexedGeoFields.size()) { continue; }

                // Finally, see if the item is in our search annulus.
                size_t geoFieldsInRange = 0;
                double minMatchingDistance = 1e20;

                // Get all the fields with that name from the document.
                BSONElementSet geoFieldElements;
                indexedObj.getFieldsDotted(_nearQuery.field, geoFieldElements, false);
                if (geoFieldElements.empty()) { continue; }

                // For each field with that name in the document...
                for (BSONElementSet::iterator oi = geoFieldElements.begin();
                        oi != geoFieldElements.end(); ++oi) {
                    if (!oi->isABSONObj()) { continue; }
                    double dist = distanceTo(oi->Obj());
                    // If it satisfies our distance criteria...
                    if (dist >= _innerRadius && dist <= _outerRadius) {
                        // Success!  For this field.
                        ++geoFieldsInRange;
                        minMatchingDistance = min(dist, minMatchingDistance);
                    }
                }
                // If all the geo query fields had something in range
                if (geoFieldsInRange > 0) {
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
