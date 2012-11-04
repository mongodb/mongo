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
#include "mongo/db/geo/s2common.h"

namespace mongo {
    // Does this GeoQueryField intersect the provided data?
    bool GeoQueryField::intersectsPoint(const S2Cell &otherPoint) {
        if (NULL != cell) {
            return cell->MayIntersect(otherPoint);
        } else if (NULL != line) {
            return line->MayIntersect(otherPoint);
        } else {
            return polygon->MayIntersect(otherPoint);
        }
    }

    bool GeoQueryField::intersectsLine(const S2Polyline& otherLine) {
        if (NULL != cell) {
            return otherLine.MayIntersect(*cell);
        } else if (NULL != line) {
            return otherLine.Intersects(line);
        } else {
            // TODO(hk): modify s2 library to just let us know if it intersected
            // rather than returning all this.
            vector<S2Polyline*> clipped;
            polygon->IntersectWithPolyline(&otherLine, &clipped);
            bool ret = clipped.size() > 0;
            for (size_t i = 0; i < clipped.size(); ++i) delete clipped[i];
            return ret;
        }
        return false;
    }

    bool GeoQueryField::intersectsPolygon(const S2Polygon& otherPolygon) {
        if (NULL != cell) {
            return otherPolygon.MayIntersect(*cell);
        } else if (NULL != line) {
            // TODO(hk): modify s2 library to just let us know if it intersected
            // rather than returning all this.
            vector<S2Polyline*> clipped;
            otherPolygon.IntersectWithPolyline(line, &clipped);
            bool ret = clipped.size() > 0;
            for (size_t i = 0; i < clipped.size(); ++i) delete clipped[i];
            return ret;
        } else {
            return otherPolygon.Intersects(polygon);
        }
    }

    const S2Region& GeoQueryField::getRegion() const {
        if (NULL != cell) {
            return *cell;
        } else if (NULL != line) {
            return *line;
        } else {
            verify(NULL != polygon);
            return *polygon;
        }
    }

    void GeoQueryField::free() {
        if (NULL != cell) {
            delete cell;
        } else if (NULL != line) {
            delete line;
        } else {
            verify(NULL != polygon);
            delete polygon;
        }
    }

    S2Cursor::S2Cursor(const BSONObj &keyPattern, const IndexDetails *details,
                       const BSONObj &query, const vector<GeoQueryField> &fields,
                       const S2IndexingParams &params, int numWanted)
        : _details(details), _fields(fields), _params(params),
          _keyPattern(keyPattern), _numToReturn(numWanted) {
        BSONObjBuilder geoFieldsToNuke;
        for (size_t i = 0; i < _fields.size(); ++i) {
            geoFieldsToNuke.append(_fields[i].field, "");
        }
        // false means we want to filter OUT geoFieldsToNuke, not filter to include only that.
        _filteredQuery = query.filterFieldsUndotted(geoFieldsToNuke.obj(), false);
        _matcher.reset(new CoveredIndexMatcher(_filteredQuery, keyPattern));
    }

    S2Cursor::~S2Cursor() {
        // We own these pointers.
        for (size_t i = 0; i < _fields.size(); ++i) {
            _fields[i].free();
        }
    }

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
            FieldRangeSet frs(_details->parentNS().c_str(), makeUnifiedFRS(), false, false);
            shared_ptr<FieldRangeVector> frv(new FieldRangeVector(frs, specForFRV, 1));
            _btreeCursor.reset(BtreeCursor::make(nsdetails(_details->parentNS().c_str()),
                                                 *_details, frv, 0, 1));
            return advance();
        }
        return _btreeCursor->ok();
    }

    // Make the FieldRangeSet of keys we look for.  Here is an example:
    // regularfield1: regularvalue1, $or [ { geo1 : {$in [parentcover1, ... ]}},
    //                                     { geo1 : {regex: ^cover1 } },
    //                                     { geo1 : {regex: ^cover2 } },
    // As for what we put into the geo field, see lengthy comments below.
    BSONObj S2Cursor::makeUnifiedFRS() {
        BSONObjBuilder frsObjBuilder;
        frsObjBuilder.appendElements(_filteredQuery);

        S2RegionCoverer coverer;
        _params.configureCoverer(&coverer);

        for (size_t i = 0; i < _fields.size(); ++i) {
            // Get a set of coverings.  We look for keys that have this covering as a prefix
            // (meaning the key is contained within the covering).
            vector<S2CellId> cover;
            coverer.GetCovering(_fields[i].getRegion(), &cover);

            BSONArrayBuilder orBuilder;

            // Look at the cells we cover (and any cells they cover via prefix).  Examine
            // everything which has our cover as a strict prefix of its key.  Anything with our
            // cover as a strict prefix is contained within the cover and should be intersection
            // tested.
            for (size_t j = 0; j < cover.size(); ++j) {
                string regex = "^" + cover[j].toString();
                orBuilder.append(BSON(_fields[i].field << BSON("$regex" << regex)));
            }

            // Look at the cells that cover us.  We want to look at every cell that contains the
            // covering we would index on.  We generate the would-index-with-this-covering and
            // find all the cells strictly containing the cells in that set, until we hit the
            // coarsest indexed cell.  We use $in, not a prefix match.  Why not prefix?  Because
            // we've already looked at everything finer or as fine as our initial covering.
            //
            // Say we have a fine point with cell id 212121, we go up one, get 21212, we don't
            // want to look at cells 21212[not-1] because we know they're not going to intersect
            // with 212121, but entries inserted with cell value 21212 (no trailing digits) may.
            // And we've already looked at points with the cell id 211111 from the regex search
            // created above, so we only want things where the value of the last digit is not
            // stored (and therefore could be 1).
            set<S2CellId> parentCells;
            for (size_t j = 0; j < cover.size(); ++j) {
                for (S2CellId id = cover[j].parent();
                     id.level() >= _params.coarsestIndexedLevel; id = id.parent()) {
                    parentCells.insert(id);
                }
            }

            // Create the actual $in statement.
            BSONArrayBuilder inBuilder;
            for (set<S2CellId>::const_iterator it = parentCells.begin();
                 it != parentCells.end(); ++it) {
                inBuilder.append(it->toString());
            }
            orBuilder.append(BSON(_fields[i].field << BSON("$in" << inBuilder.arr())));

            // Join the regexes with the in statement via an or.
            // TODO(hk): see if this actually works with two geo fields or if they have
            // to be joined with an and or what.
            frsObjBuilder.append("$or", orBuilder.arr());
        }
        return frsObjBuilder.obj();
    }

    Record* S2Cursor::_current() { return _btreeCursor->currLoc().rec(); }
    BSONObj S2Cursor::current() { return _btreeCursor->currLoc().obj(); }
    DiskLoc S2Cursor::currLoc() { return _btreeCursor->currLoc(); }
    BSONObj S2Cursor::currKey() const { return _btreeCursor->currKey(); }
    DiskLoc S2Cursor::refLoc() { return DiskLoc(); }
    long long S2Cursor::nscanned() { return _nscanned; }

    // This is the actual search.
    bool S2Cursor::advance() {
        if (_numToReturn <= 0) { return false; }
        for (; _btreeCursor->ok(); _btreeCursor->advance()) {
            if (_seen.end() != _seen.find(_btreeCursor->currLoc())) { continue; }
            _seen.insert(_btreeCursor->currLoc());
            ++_nscanned;

            MatchDetails details;
            bool matched = _matcher->matchesCurrent(_btreeCursor.get(), &details);
            if (!matched) { continue; }

            const BSONObj &indexedObj = _btreeCursor->currLoc().obj();

            size_t geoFieldsMatched = 0;
            // OK, cool, non-geo match satisfied.  See if the object actually overlaps w/the geo
            // query fields.
            for (size_t i = 0; i < _fields.size(); ++i) {
                BSONElementSet geoFieldElements;
                indexedObj.getFieldsDotted(_fields[i].field, geoFieldElements);
                if (geoFieldElements.empty()) { continue; }

                bool match = false;

                for (BSONElementSet::iterator oi = geoFieldElements.begin();
                     oi != geoFieldElements.end(); ++oi) {
                    const BSONObj &geoObj = oi->Obj();
                    if (GeoJSONParser::isPolygon(geoObj)) {
                        S2Polygon shape;
                        GeoJSONParser::parsePolygon(geoObj, &shape);
                        match = _fields[i].intersectsPolygon(shape);
                    } else if (GeoJSONParser::isLineString(geoObj)) {
                        S2Polyline shape;
                        GeoJSONParser::parseLineString(geoObj, &shape);
                        match = _fields[i].intersectsLine(shape);
                    } else if (GeoJSONParser::isPoint(geoObj)) {
                        S2Cell point;
                        GeoJSONParser::parsePoint(geoObj, &point);
                        match = _fields[i].intersectsPoint(point);
                    }
                    if (match) break;
                }

                if (match) { ++geoFieldsMatched; }
            }

            if (geoFieldsMatched == _fields.size()) {
                // We have a winner!  And we point at it.
                --_numToReturn;
                return true;
            }
        }
        return false;
    }

    // TODO: yielding is very un-tested.
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
        // TODO(hk): Dump more meaningful stats.
        b << "nscanned " << _nscanned;
    }
}  // namespace mongo
