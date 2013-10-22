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

#include "mongo/db/exec/2dcommon.h"
#include "mongo/db/query/index_bounds_builder.h"

namespace mongo {
namespace twod_exec {

    //
    // GeoAccumulator
    //

    GeoAccumulator::GeoAccumulator(TwoDAccessMethod* accessMethod, MatchExpression* filter, bool uniqueDocs,
            bool needDistance)
        : _accessMethod(accessMethod), _converter(accessMethod->getParams().geoHashConverter),
        _lookedAt(0), _matchesPerfd(0), _objectsLoaded(0), _pointsLoaded(0), _found(0),
        _uniqueDocs(uniqueDocs), _needDistance(needDistance) {

            _filter = filter;
        }

    GeoAccumulator::~GeoAccumulator() { }

    void GeoAccumulator::add(const GeoIndexEntry& node) {
        _lookedAt++;

        // Approximate distance check using key data
        double keyD = 0;
        Point keyP(_converter->unhashToPoint(node._key.firstElement()));
        KeyResult keyOk = approxKeyCheck(keyP, keyD);
        if (keyOk == BAD) {
            return;
        }

        // Check for match using other key (and potentially doc) criteria
        // Remember match results for each object
        map<DiskLoc, bool>::iterator match = _matched.find(node.recordLoc);
        bool newDoc = match == _matched.end();

        //cout << "newDoc: " << newDoc << endl;
        if(newDoc) {
            if (NULL != _filter) {
                // XXX: use key information to match...shove in WSM, try loc_and_idx, then fetch obj
                // and try that.
                BSONObj obj = node.recordLoc.obj();
                bool good = _filter->matchesBSON(obj, NULL);
                _matchesPerfd++;

                //if (details.hasLoadedRecord())
                //_objectsLoaded++;

                if (! good) {
                    _matched[ node.recordLoc ] = false;
                    return;
                }
            }
            _matched[ node.recordLoc ] = true;
        } else if(!((*match).second)) {
            return;
        }

        // Exact check with particular data fields
        // Can add multiple points
        int diff = addSpecific(node, keyP, keyOk == BORDER, keyD, newDoc);
        if(diff > 0) _found += diff;
        else _found -= -diff;
    }

    void GeoAccumulator::getPointsFor(const BSONObj& key, const BSONObj& obj,
            vector<BSONObj> &locsForNode, bool allPoints) {
        // Find all the location objects from the keys
        vector<BSONObj> locs;
        _accessMethod->getKeys(obj, allPoints ? locsForNode : locs);
        ++_pointsLoaded;

        if (allPoints) return;
        if (locs.size() == 1){
            locsForNode.push_back(locs[0]);
            return;
        }

        // Find the particular location we want
        GeoHash keyHash(key.firstElement(), _converter->getBits());

        for(vector< BSONObj >::iterator i = locs.begin(); i != locs.end(); ++i) {
            // Ignore all locations not hashed to the key's hash, since we may see
            // those later
            if(_converter->hash(*i) != keyHash) continue;
            locsForNode.push_back(*i);
        }
    }

    //
    // BtreeLocation
    //

    // static
    bool BtreeLocation::hasPrefix(const BSONObj& key, const GeoHash& hash) {
        BSONElement e = key.firstElement();
        if (e.eoo()) { return false; }
        return GeoHash(e).hasPrefix(hash);
    }

    void BtreeLocation::advance() {
        WorkingSetID id = WorkingSet::INVALID_ID;
        for (;;) {
            PlanStage::StageState state = _scan->work(&id);
            if (PlanStage::ADVANCED == state) {
                break;
            }
            else if (PlanStage::NEED_TIME == state) {
                continue;
            }
            else {
                // Error or EOF.  Either way, stop.
                _eof = true;
                return;
            }
        }
        verify(WorkingSet::INVALID_ID != id);
        WorkingSetMember* wsm = _ws->get(id);
        verify(WorkingSetMember::LOC_AND_IDX == wsm->state);
        _key = wsm->keyData[0].keyData;
        _loc = wsm->loc;
        _ws->free(id);
    }

    // Returns the min and max keys which bound a particular location.
    // The only time these may be equal is when we actually equal the location
    // itself, otherwise our expanding algorithm will fail.
    // static
    bool BtreeLocation::initial(IndexDescriptor* descriptor, const TwoDIndexingParams& params,
            BtreeLocation& min, BtreeLocation& max, GeoHash start) {
        verify(descriptor);

        min._eof = false;
        max._eof = false;

        // Add the range for the 2d indexed field to the keys used.

        // Two scans: one for min one for max.
        IndexScanParams minParams;
        minParams.direction = -1;
        minParams.forceBtreeAccessMethod = true;
        minParams.descriptor = descriptor->clone();
        minParams.bounds.fields.resize(descriptor->keyPattern().nFields());
        minParams.doNotDedup = true;
        // First field of start key goes (MINKEY, start] (in reverse)
        BSONObjBuilder firstBob;
        firstBob.appendMinKey("");
        start.appendToBuilder(&firstBob, "");
        minParams.bounds.fields[0].intervals.push_back(Interval(firstBob.obj(), false, true));

        IndexScanParams maxParams;
        maxParams.forceBtreeAccessMethod = true;
        maxParams.direction = 1;
        maxParams.descriptor = descriptor->clone();
        maxParams.bounds.fields.resize(descriptor->keyPattern().nFields());
        // Don't have the ixscan dedup since we want dup DiskLocs because of multi-point docs.
        maxParams.doNotDedup = true;
        // First field of end key goes (start, MAXKEY)
        BSONObjBuilder secondBob;
        start.appendToBuilder(&secondBob, "");
        secondBob.appendMaxKey("");
        maxParams.bounds.fields[0].intervals.push_back(Interval(secondBob.obj(), false, false));

        BSONObjIterator it(descriptor->keyPattern());
        BSONElement kpElt = it.next();
        maxParams.bounds.fields[0].name = kpElt.fieldName();
        minParams.bounds.fields[0].name = kpElt.fieldName();
        // Fill out the non-2d indexed fields with the "all values" interval, aligned properly.
        size_t idx = 1;
        while (it.more()) {
            kpElt = it.next();
            maxParams.bounds.fields[idx].intervals.push_back(IndexBoundsBuilder::allValues());
            minParams.bounds.fields[idx].intervals.push_back(IndexBoundsBuilder::allValues());
            maxParams.bounds.fields[idx].name = kpElt.fieldName();
            minParams.bounds.fields[idx].name = kpElt.fieldName();
            if (kpElt.number() == -1) {
                IndexBoundsBuilder::reverseInterval(&minParams.bounds.fields[idx].intervals[0]);
                IndexBoundsBuilder::reverseInterval(&maxParams.bounds.fields[idx].intervals[0]);
            }
            ++idx;
        }

        for (size_t i = 0; i < minParams.bounds.fields.size(); ++i) {
            IndexBoundsBuilder::reverseInterval(&minParams.bounds.fields[i].intervals[0]);
        }

        //cout << "keyPattern " << descriptor->keyPattern().toString() << endl;
        //cout << "minBounds " << minParams.bounds.toString() << endl;
        //cout << "maxBounds " << maxParams.bounds.toString() << endl;
        verify(minParams.bounds.isValidFor(descriptor->keyPattern(), -1));
        verify(maxParams.bounds.isValidFor(descriptor->keyPattern(), 1));

        min._ws.reset(new WorkingSet());
        min._scan.reset(new IndexScan(minParams, min._ws.get(), NULL));

        max._ws.reset(new WorkingSet());
        max._scan.reset(new IndexScan(maxParams, max._ws.get(), NULL));

        min.advance();
        max.advance();

        return !max._eof || !min._eof;
    }

    //
    // GeoBrowse
    //

    GeoBrowse::GeoBrowse(TwoDAccessMethod* accessMethod, string type, MatchExpression* filter, bool uniqueDocs, bool needDistance)
        : GeoAccumulator(accessMethod, filter, uniqueDocs, needDistance),
        _type(type), _firstCall(true), _nscanned(),
        _centerPrefix(0, 0, 0),
        _descriptor(accessMethod->getDescriptor()),
        _converter(accessMethod->getParams().geoHashConverter),
        _params(accessMethod->getParams()) {

            // Set up the initial expand state
            _state = START;
            _neighbor = -1;
            _foundInExp = 0;

        }

    bool GeoBrowse::ok() {
        /*
       cout << "Checking cursor, in state " << (int) _state << ", first call "
       << _firstCall << ", empty : " << _cur.isEmpty()
       << ", stack : " << _stack.size() << endl;
       */

        bool first = _firstCall;

        if (_firstCall) {
            fillStack(maxPointsHeuristic);
            _firstCall = false;
        }

        if (_stack.size()) {
            if (first) { ++_nscanned; }
            return true;
        }

        while (moreToDo()) {
            fillStack(maxPointsHeuristic);
            if (! _cur.isEmpty()) {
                if (first) { ++_nscanned; }
                return true;
            }
        }

        return !_cur.isEmpty();
    }

    bool GeoBrowse::advance() {
        _cur._o = BSONObj();

        if (_stack.size()) {
            _cur = _stack.front();
            _stack.pop_front();
            ++_nscanned;
            return true;
        }

        if (! moreToDo()) return false;

        while (_cur.isEmpty() && moreToDo()){
            fillStack(maxPointsHeuristic);
        }

        return ! _cur.isEmpty() && ++_nscanned;
    }

    void GeoBrowse::noteLocation() {
        // Remember where our _max, _min are
        _min.prepareToYield();
        _max.prepareToYield();
    }

    /* called before query getmore block is iterated */
    void GeoBrowse::checkLocation() {
        // We can assume an error was thrown earlier if this database somehow disappears
        // Recall our _max, _min
        _min.recoverFromYield();
        _max.recoverFromYield();
    }

    Record* GeoBrowse::_current() { verify(ok()); return _cur._loc.rec(); }
    BSONObj GeoBrowse::current() { verify(ok()); return _cur._o; }
    DiskLoc GeoBrowse::currLoc() { verify(ok()); return _cur._loc; }
    BSONObj GeoBrowse::currKey() const { return _cur._key; }

    // Are we finished getting points?
    bool GeoBrowse::moreToDo() { return _state != DONE; }

    Box GeoBrowse::makeBox(const GeoHash &hash) const {
        double sizeEdge = _converter->sizeEdge(hash);
        Point min(_converter->unhashToPoint(hash));
        Point max(min.x + sizeEdge, min.y + sizeEdge);
        return Box(min, max);
    }

    bool GeoBrowse::checkAndAdvance(BtreeLocation* bl, const GeoHash& hash, int& totalFound) {
        if (bl->eof()) { return false; }

        //cout << "looking at " << bl->_loc.obj().toString() << " dl " << bl->_loc.toString() << endl;

        if (!BtreeLocation::hasPrefix(bl->_key, hash)) { return false; }

        totalFound++;
        GeoIndexEntry n(bl->_loc, bl->_key);
        add(n);
        //cout << "adding\n";

        bl->advance();

        return true;
    }


    // Fills the stack, but only checks a maximum number of maxToCheck points at a time.
    // Further calls to this function will continue the expand/check neighbors algorithm.
    void GeoBrowse::fillStack(int maxToCheck, int maxToAdd, bool onlyExpand) {
        if(maxToAdd < 0) maxToAdd = maxToCheck;
        int maxFound = _foundInExp + maxToCheck;
        verify(maxToCheck > 0);
        verify(maxFound > 0);
        verify(_found <= 0x7fffffff); // conversion to int
        int maxAdded = static_cast<int>(_found) + maxToAdd;
        verify(maxAdded >= 0); // overflow check

        bool isNeighbor = _centerPrefix.constrains();

        // Starting a box expansion
        if (_state == START) {
            // Get the very first hash point, if required
            if(! isNeighbor)
                _prefix = expandStartHash();

            if (!BtreeLocation::initial(_descriptor, _params, _min, _max, _prefix)) {
                _state = isNeighbor ? DONE_NEIGHBOR : DONE;
            } else {
                _state = DOING_EXPAND;
                _lastPrefix.reset();
            }
        }

        // Doing the actual box expansion
        if (_state == DOING_EXPAND) {
            while (true) {
                // Record the prefix we're actively exploring...
                _expPrefix.reset(new GeoHash(_prefix));

                // Find points inside this prefix
                while (checkAndAdvance(&_min, _prefix, _foundInExp)
                        && _foundInExp < maxFound && _found < maxAdded) {}
                while (checkAndAdvance(&_max, _prefix, _foundInExp)
                        && _foundInExp < maxFound && _found < maxAdded) {}

                if(_foundInExp >= maxFound || _found >= maxAdded) return;

                // We've searched this prefix fully, remember
                _lastPrefix.reset(new GeoHash(_prefix));

                // If we've searched the entire space, we're finished.
                if (! _prefix.constrains()) {
                    _state = DONE;
                    notePrefix();
                    return;
                }

                // If we won't fit in the box, and we're not doing a sub-scan, increase the size
                if (! fitsInBox(_converter->sizeEdge(_prefix)) && _fringe.size() == 0) {
                    // If we're still not expanded bigger than the box size, expand again
                    _prefix = _prefix.up();
                    continue;
                }

                // We're done and our size is large enough
                _state = DONE_NEIGHBOR;

                // Go to the next sub-box, if applicable
                if(_fringe.size() > 0) _fringe.pop_back();
                // Go to the next neighbor if this was the last sub-search
                if(_fringe.size() == 0) _neighbor++;
                break;
            }
            notePrefix();
        }

        // If we doeighbors
        if(onlyExpand) return;

        // If we're done expanding the current box...
        if(_state == DONE_NEIGHBOR) {
            // Iterate to the next neighbor
            // Loop is useful for cases where we want to skip over boxes entirely,
            // otherwise recursion increments the neighbors.
            for (; _neighbor < 9; _neighbor++) {
                // If we have no fringe for the neighbor, make sure we have the default fringe
                if(_fringe.size() == 0) _fringe.push_back("");

                if(! isNeighbor) {
                    _centerPrefix = _prefix;
                    _centerBox = makeBox(_centerPrefix);
                    isNeighbor = true;
                }

                int i = (_neighbor / 3) - 1;
                int j = (_neighbor % 3) - 1;

                if ((i == 0 && j == 0) ||
                        (i < 0 && _centerPrefix.atMinX()) ||
                        (i > 0 && _centerPrefix.atMaxX()) ||
                        (j < 0 && _centerPrefix.atMinY()) ||
                        (j > 0 && _centerPrefix.atMaxY())) {

                    continue; // main box or wrapped edge
                    // TODO:  We may want to enable wrapping in future, probably best as layer
                    // on top of this search.
                }

                // Make sure we've got a reasonable center
                verify(_centerPrefix.constrains());

                GeoHash _neighborPrefix = _centerPrefix;
                _neighborPrefix.move(i, j);

                while(_fringe.size() > 0) {
                    _prefix = _neighborPrefix + _fringe.back();
                    Box cur(makeBox(_prefix));

                    double intAmt = intersectsBox(cur);

                    // No intersection
                    if(intAmt <= 0) {
                        _fringe.pop_back();
                        continue;
                    } else if(intAmt < 0.5 && _prefix.canRefine()
                            && _fringe.back().size() < 4 /* two bits */) {
                        // Small intersection, refine search
                        string lastSuffix = _fringe.back();
                        _fringe.pop_back();
                        _fringe.push_back(lastSuffix + "00");
                        _fringe.push_back(lastSuffix + "01");
                        _fringe.push_back(lastSuffix + "11");
                        _fringe.push_back(lastSuffix + "10");
                        continue;
                    }

                    // Restart our search from a diff box.
                    _state = START;
                    verify(! onlyExpand);
                    verify(_found <= 0x7fffffff);
                    fillStack(maxFound - _foundInExp, maxAdded - static_cast<int>(_found));
                    // When we return from the recursive fillStack call, we'll either have
                    // checked enough points or be entirely done.  Max recurse depth is < 8 *
                    // 16.
                    // If we're maxed out on points, return
                    if(_foundInExp >= maxFound || _found >= maxAdded) {
                        // Make sure we'll come back to add more points
                        verify(_state == DOING_EXPAND);
                        return;
                    }

                    // Otherwise we must be finished to return
                    verify(_state == DONE);
                    return;
                }
            }
            // Finished with neighbors
            _state = DONE;
        }
    }

    bool GeoBrowse::remembered(BSONObj o){
        BSONObj seenId = o["_id"].wrap("").getOwned();
        if(_seenIds.find(seenId) != _seenIds.end()){
            return true;
        } else{
            _seenIds.insert(seenId);
            return false;
        }
    }

    int GeoBrowse::addSpecific(const GeoIndexEntry& node, const Point& keyP, bool onBounds,
            double keyD, bool potentiallyNewDoc) {
        int found = 0;
        // We need to handle every possible point in this method, even those not in the key
        // value, to avoid us tracking which hashes we've already seen.
        if(! potentiallyNewDoc){ return 0; }

        // Final check for new doc
        // OK to touch, since we're probably returning this object now
        if(remembered(node.recordLoc.obj())) {
            //cout << "remembered\n";
            return 0;
        }

        if(_uniqueDocs && ! onBounds) {
            //log() << "Added ind to " << _type << endl;
            _stack.push_front(GeoPoint(node));
            found++;
        } else {
            // We now handle every possible point in the document, even those not in the key
            // value, since we're iterating through them anyway - prevents us from having to
            // save the hashes we've seen per-doc
            // If we're filtering by hash, get the original

            vector< BSONObj > locs;
            getPointsFor(node._key, node.recordLoc.obj(), locs, true);
            for(vector< BSONObj >::iterator i = locs.begin(); i != locs.end(); ++i){
                double d = -1;
                Point p(*i);
                // We can avoid exact document checks by redoing approx checks,
                // if the exact checks are more expensive.
                bool needExact = true;

                if(! needExact || exactDocCheck(p, d)){
                    //log() << "Added mult to " << _type << endl;
                    _stack.push_front(GeoPoint(node));
                    found++;
                    // If returning unique, just exit after first point is added
                    if(_uniqueDocs) break;
                }
            }
        }

        while(_cur.isEmpty() && _stack.size() > 0){
            _cur = _stack.front();
            _stack.pop_front();
        }

        return found;
    }

    long long GeoBrowse::nscanned() {
        if (_firstCall) { ok(); }
        return _nscanned;
    }

    void GeoBrowse::explainDetails(BSONObjBuilder& b){
        b << "lookedAt" << _lookedAt;
        b << "matchesPerfd" << _matchesPerfd;
        b << "objectsLoaded" << _objectsLoaded;
        b << "pointsLoaded" << _pointsLoaded;
        // b << "pointsSavedForYield" << _nDirtied;
        // b << "pointsChangedOnYield" << _nChangedOnYield;
        // b << "pointsRemovedOnYield" << _nRemovedOnYield;
    }

    void GeoBrowse::invalidate(const DiskLoc& dl) {
        if (_firstCall) { return; }

        if (_cur._loc == dl) {
            advance();
        }

        list<GeoPoint>::iterator it = _stack.begin();
        while (it != _stack.end()) {
            if (it->_loc == dl) {
                list<GeoPoint>::iterator old = it;
                it++;
                _stack.erase(old);
            }
            else {
                it++;
            }
        }

        if (!_min.eof() && _min._loc == dl) {
            _min.recoverFromYield();
            while (_min._loc == dl && !_min.eof()) {
                _min.advance();
            }
            _min.prepareToYield();
        }

        if (!_max.eof() && _max._loc == dl) {
            _max.recoverFromYield();
            while (_max._loc == dl && !_max.eof()) {
                _max.advance();
            }
            _max.prepareToYield();
        }
    }

}  // namespace twod_exec
}  // namespace mongo
