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

#include "mongo/db/index/s2_simple_cursor.h"

#include "mongo/db/btreecursor.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/queryutil.h"
#include "third_party/s2/s2regionintersection.h"

namespace mongo {

    S2SimpleCursor::S2SimpleCursor(IndexDescriptor* descriptor, const S2IndexingParams& params)
        : _descriptor(descriptor), _params(params) { }

    void S2SimpleCursor::seek(const BSONObj& query, const vector<GeoQuery>& regions) {
        _nscanned = 0;
        _matchTested = 0;
        _geoTested = 0;
        _fields = regions;
        _seen = unordered_set<DiskLoc, DiskLoc::Hasher>();

        BSONObjBuilder geoFieldsToNuke;
        for (size_t i = 0; i < _fields.size(); ++i) {
            geoFieldsToNuke.append(_fields[i].getField(), "");
        }
        // false means we want to filter OUT geoFieldsToNuke, not filter to include only that.
        _filteredQuery = query.filterFieldsUndotted(geoFieldsToNuke.obj(), false);

        BSONObjBuilder specBuilder;
        BSONObjIterator i(_descriptor->keyPattern());
        while (i.more()) {
            BSONElement e = i.next();
            // Checked in AccessMethod already, so we know this spec has only numbers and 2dsphere
            if ( e.type() == String ) {
                specBuilder.append( e.fieldName(), 1 );
            }
            else {
                specBuilder.append( e.fieldName(), e.numberInt() );
            }
        }
        BSONObj spec = specBuilder.obj();

        BSONObj frsObj;

        BSONObjBuilder frsObjBuilder;
        frsObjBuilder.appendElements(_filteredQuery);

        S2RegionCoverer coverer;

        for (size_t i = 0; i < _fields.size(); ++i) {
            vector<S2CellId> cover;
            double area = _fields[i].getRegion().GetRectBound().Area();
            S2SearchUtil::setCoverLimitsBasedOnArea(area, &coverer, _params.coarsestIndexedLevel);
            coverer.GetCovering(_fields[i].getRegion(), &cover);
            uassert(16759, "No cover ARGH?!", cover.size() > 0);
            _cellsInCover = cover.size();
            BSONObj fieldRange = S2SearchUtil::coverAsBSON(cover, _fields[i].getField(),
                    _params.coarsestIndexedLevel);
            frsObjBuilder.appendElements(fieldRange);
        }

        frsObj = frsObjBuilder.obj();

        FieldRangeSet frs(_descriptor->parentNS().c_str(), frsObj, false, false);
        shared_ptr<FieldRangeVector> frv(new FieldRangeVector(frs, spec, 1));
        _btreeCursor.reset(BtreeCursor::make(nsdetails(_descriptor->parentNS()),
                                             _descriptor->getOnDisk(), frv, 0, 1));
        next();
    }

    Status S2SimpleCursor::seek(const BSONObj& position) {
        return Status::OK();
    }

    Status S2SimpleCursor::setOptions(const CursorOptions& options) {
        return Status::OK();
    }

    bool S2SimpleCursor::isEOF() const { return !_btreeCursor->ok(); }
    BSONObj S2SimpleCursor::getKey() const { return _btreeCursor->currKey(); }
    DiskLoc S2SimpleCursor::getValue() const { return _btreeCursor->currLoc(); }

    void S2SimpleCursor::next() {
        for (; _btreeCursor->ok(); _btreeCursor->advance()) {
            ++_nscanned;
            if (_seen.end() != _seen.find(_btreeCursor->currLoc())) { continue; }
            _seen.insert(_btreeCursor->currLoc());

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
                    uassert(16760, "malformed geometry: " + geoObj.toString(),
                            geoContainer.parseFrom(geoObj));
                    match = _fields[i].satisfiesPredicate(geoContainer);
                }

                if (match) { ++geoFieldsMatched; }
            }

            if (geoFieldsMatched == _fields.size()) {
                // We have a winner!  And we point at it.
                return;
            }
        }
    }

    string S2SimpleCursor::toString() { return "S2Cursor: " + _btreeCursor->toString(); }

    Status S2SimpleCursor::savePosition() {
        _btreeCursor->noteLocation();
        _seen.clear();
        return Status::OK();
    }

    Status S2SimpleCursor::restorePosition() {
        _btreeCursor->checkLocation();
        // We are pointing at a valid btree location now, but it may not be a valid result.
        // This ensures that we're pointing at a valid result that satisfies the query.

        // There is something subtle here: Say we point at something valid, and note the location
        // (yield), then checkLocation (unyield), when we call advance, we don't go past the object
        // that we were/are pointing at since we only do that if we've seen it before (that is, it's
        // in _seen, which we clear when we yield).
        next();
        return Status::OK();
    }

}  // namespace mongo
