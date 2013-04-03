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

#include "mongo/db/geo/s2cursor.h"

#include "mongo/db/btreecursor.h"
#include "mongo/db/index.h"
#include "mongo/db/matcher.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/queryutil.h"
#include "mongo/db/geo/s2common.h"

namespace mongo {
    S2Cursor::S2Cursor(const BSONObj &keyPattern, const IndexDetails *details,
                       const BSONObj &query, const vector<GeoQuery> &fields,
                       const S2IndexingParams &params)
        : _details(details), _fields(fields), _params(params), _keyPattern(keyPattern),
          _nscanned(0), _matchTested(0), _geoTested(0) {

        BSONObjBuilder geoFieldsToNuke;
        for (size_t i = 0; i < _fields.size(); ++i) {
            geoFieldsToNuke.append(_fields[i].getField(), "");
        }
        // false means we want to filter OUT geoFieldsToNuke, not filter to include only that.
        _filteredQuery = query.filterFieldsUndotted(geoFieldsToNuke.obj(), false);
        _matcher.reset(new CoveredIndexMatcher(_filteredQuery, keyPattern));
    }

    S2Cursor::~S2Cursor() { }

    CoveredIndexMatcher* S2Cursor::matcher() const { return _matcher.get(); }

    bool S2Cursor::ok() {
        if (NULL == _btreeCursor.get()) {
            // FieldRangeVector needs an IndexSpec so we make it one.
            BSONObjBuilder specBuilder;
            BSONObjIterator i(_keyPattern);
            while (i.more()) {
                BSONElement e = i.next();
                specBuilder.append(e.fieldName(), 1);
            }
            BSONObj spec = specBuilder.obj();
            IndexSpec specForFRV(spec);
            // All the magic is in makeUnifiedFRS.  See below.
            // A lot of these arguments are opaque.
            BSONObj frsObj;
            if (!makeFRSObject(&frsObj)) { return false; }
            FieldRangeSet frs(_details->parentNS().c_str(), frsObj, false, false);
            shared_ptr<FieldRangeVector> frv(new FieldRangeVector(frs, specForFRV, 1));
            _btreeCursor.reset(BtreeCursor::make(nsdetails(_details->parentNS()),
                                                 *_details, frv, 0, 1));
            return advance();
        }
        return _btreeCursor->ok();
    }

    // Make the FieldRangeSet of keys we look for.  Uses coverAsBSON to go from
    // a region to a covering to a set of keys for that covering.
    // Returns false if the FRS object would be empty.
    bool S2Cursor::makeFRSObject(BSONObj *out) {
        BSONObjBuilder frsObjBuilder;
        frsObjBuilder.appendElements(_filteredQuery);

        S2RegionCoverer coverer;

        for (size_t i = 0; i < _fields.size(); ++i) {
            vector<S2CellId> cover;
            double area = _fields[i].getRegion().GetRectBound().Area();
            S2SearchUtil::setCoverLimitsBasedOnArea(area, &coverer, _params.coarsestIndexedLevel);
            coverer.GetCovering(_fields[i].getRegion(), &cover);
            if (0 == cover.size()) { return false; }
            _cellsInCover = cover.size();
            BSONObj fieldRange = S2SearchUtil::coverAsBSON(cover, _fields[i].getField(),
                _params.coarsestIndexedLevel);
            frsObjBuilder.appendElements(fieldRange);
        }

        *out = frsObjBuilder.obj();
        return true;
    }

    Record* S2Cursor::_current() { return _btreeCursor->currLoc().rec(); }
    BSONObj S2Cursor::current() { return _btreeCursor->currLoc().obj(); }
    DiskLoc S2Cursor::currLoc() { return _btreeCursor->currLoc(); }
    BSONObj S2Cursor::currKey() const { return _btreeCursor->currKey(); }
    DiskLoc S2Cursor::refLoc() { return DiskLoc(); }
    long long S2Cursor::nscanned() { return _nscanned; }
    bool S2Cursor::getsetdup(DiskLoc loc) { return _btreeCursor->getsetdup(loc); }
    void S2Cursor::aboutToDeleteBucket(const DiskLoc& b) {
        if (NULL != _btreeCursor) {
            _btreeCursor->aboutToDeleteBucket(b);
        }
    }

    // This is the actual search.
    bool S2Cursor::advance() {
        for (; _btreeCursor->ok(); _btreeCursor->advance()) {
            ++_nscanned;
            if (_seen.end() != _seen.find(_btreeCursor->currLoc())) { continue; }
            _seen.insert(_btreeCursor->currLoc());

            ++_matchTested;
            MatchDetails details;
            bool matched = _matcher->matchesCurrent(_btreeCursor.get(), &details);
            if (!matched) { continue; }

            const BSONObj &indexedObj = _btreeCursor->currLoc().obj();

            ++_geoTested;
            size_t geoFieldsMatched = 0;
            // OK, cool, non-geo match satisfied.  See if the object actually overlaps w/the geo
            // query fields.
            for (size_t i = 0; i < _fields.size(); ++i) {
                BSONElementSet geoFieldElements;
                indexedObj.getFieldsDotted(_fields[i].getField(), geoFieldElements, false);
                if (geoFieldElements.empty()) { continue; }

                bool match = false;

                for (BSONElementSet::iterator oi = geoFieldElements.begin();
                     !match && (oi != geoFieldElements.end()); ++oi) {
                    if (!oi->isABSONObj()) { continue; }
                    const BSONObj &geoObj = oi->Obj();
                    GeometryContainer geoContainer;
                    uassert(16698, "malformed geometry: " + geoObj.toString(),
                            geoContainer.parseFrom(geoObj));
                    match = _fields[i].satisfiesPredicate(geoContainer);
                }

                if (match) { ++geoFieldsMatched; }
            }

            if (geoFieldsMatched == _fields.size()) {
                // We have a winner!  And we point at it.
                return true;
            }
        }
        return false;
    }

    // This is called when we're supposed to yield.
    void S2Cursor::noteLocation() {
        _btreeCursor->noteLocation();
        _seen.clear();
    }

    // Called when we're un-yielding.
    void S2Cursor::checkLocation() {
        _btreeCursor->checkLocation();
        // We are pointing at a valid btree location now, but it may not be a valid result.
        // This ensures that we're pointing at a valid result that satisfies the query.

        // There is something subtle here: Say we point at something valid, and note the location
        // (yield), then checkLocation (unyield), when we call advance, we don't go past the object
        // that we were/are pointing at since we only do that if we've seen it before (that is, it's
        // in _seen, which we clear when we yield).
        advance();
    }

    void S2Cursor::explainDetails(BSONObjBuilder& b) {
        b << "nscanned" << _nscanned;
        b << "matchTested" << _matchTested;
        b << "geoTested" << _geoTested;
        b << "cellsInCover" << _cellsInCover;
    }
}  // namespace mongo
