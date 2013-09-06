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

#include "mongo/db/index/2d_index_cursor.h"

#ifdef _WIN32
#include <float.h>
#define nextafter _nextafter
#else
#include <cmath> // nextafter
#endif

#include "mongo/db/btreecursor.h"
#include "mongo/db/index/2d_access_method.h"
#include "mongo/db/index/btree_interface.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/geo/core.h"
#include "mongo/db/geo/geonear.h"
#include "mongo/db/geo/hash.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/queryutil.h"

namespace mongo {

    // All these internal classes exist in namespace mongo until we kill the 2d index type.
    // For now, put them into their own namespace to avoid scary "which symbol are we using" issues.
    namespace twod_internal {

    enum GeoDistType {
        GEO_PLANE,
        GEO_SPHERE
    };

    class GeoKeyNode { 
    public:
        GeoKeyNode(DiskLoc bucket, int keyOfs, DiskLoc r, BSONObj k)
            : _bucket(bucket), _keyOfs(keyOfs), recordLoc(r), _key(k) { }
        const DiskLoc _bucket;
        const int _keyOfs;
        const DiskLoc recordLoc;
        const BSONObj _key;
    private:
        GeoKeyNode();
    };

    inline double computeXScanDistance(double y, double maxDistDegrees) {
        // TODO: this overestimates for large maxDistDegrees far from the equator
        return maxDistDegrees / min(cos(deg2rad(min(+89.0, y + maxDistDegrees))),
                                    cos(deg2rad(max(-89.0, y - maxDistDegrees))));
    }

    class GeoPoint {
    public:
        GeoPoint() : _distance(-1), _exact(false), _dirty(false) { }

        //// Distance not used ////

        GeoPoint(const GeoKeyNode& node)
            : _key(node._key), _loc(node.recordLoc), _o(node.recordLoc.obj()),
              _distance(-1), _exact(false), _dirty(false), _bucket(node._bucket),
              _pos(node._keyOfs) { }

        //// Immediate initialization of distance ////

        GeoPoint(const GeoKeyNode& node, double distance, bool exact)
            : _key(node._key), _loc(node.recordLoc), _o(node.recordLoc.obj()),
              _distance(distance), _exact(exact), _dirty(false) { }

        GeoPoint(const GeoPoint& pt, double distance, bool exact)
            : _key(pt.key()), _loc(pt.loc()), _o(pt.obj()), _distance(distance), _exact(exact),
             _dirty(false) { }

        bool operator<(const GeoPoint& other) const {
            if (_distance != other._distance) return _distance < other._distance;
            if (_exact != other._exact) return _exact < other._exact;
            return _loc < other._loc;
        }

        double distance() const { return _distance; }
        bool isExact() const { return _exact; }
        BSONObj key() const { return _key; }
        bool hasLoc() const { return _loc.isNull(); }
        BSONObj obj() const { return _o; }
        BSONObj pt() const { return _pt; }
        bool isEmpty() { return _o.isEmpty(); }
        bool isCleanAndEmpty() { return isEmpty() && !isDirty(); }
        bool isDirty(){ return _dirty; }

        DiskLoc loc() const {
            verify(!_dirty);
            return _loc;
        }

        string toString() const {
            return str::stream() << "Point from " << _key << " - " << _o
                                 << " dist : " << _distance << (_exact ? " (ex)" : " (app)");
        }

        // Recover from yield by finding all the changed disk locs here, modifying the _seenPts
        // array.  Not sure yet the correct thing to do about _seen.  Definitely need to re-find our
        // current max/min locations too
        bool unDirty(const BtreeInterface* btreeInterface, IndexDescriptor* descriptor,
                     DiskLoc& oldLoc) {
            verify(_dirty);
            verify(! _id.isEmpty());

            oldLoc = _loc;
            _loc = DiskLoc();

            // Check this position and the one immediately preceding
            for(int i = 0; i < 2; i++){
                if (_pos - i < 0) continue;

                BSONObj key;
                DiskLoc loc;
                btreeInterface->keyAndRecordAt(_bucket, _pos - i, &key, &loc);

                if (loc.isNull()) continue;

                if (key.binaryEqual(_key) && loc.obj()["_id"].wrap("").binaryEqual(_id)) {
                    _pos = _pos - i;
                    _loc = loc;
                    _dirty = false;
                    _o = loc.obj();
                    return true;
                }
            }

            // Slow undirty
            scoped_ptr<BtreeCursor> cursor(BtreeCursor::make(nsdetails(descriptor->parentNS()),
                                            descriptor->getOnDisk(), _key, _key, true, 1));

            int count = 0;
            while(cursor->ok()){
                count++;
                if(cursor->current()["_id"].wrap("").binaryEqual(_id)){
                    _bucket = cursor->getBucket();
                    _pos = cursor->getKeyOfs();
                    _loc = cursor->currLoc();
                    _o = _loc.obj();
                    break;
                } else{
                    LOG(CDEBUG + 1) << "Key doesn't match : " << cursor->current()["_id"]
                                    << " saved : " << _id << endl;
                }
                cursor->advance();
            }

            if(! count) { LOG(CDEBUG) << "No key found for " << _key << endl; }
            _dirty = false;
            return _loc == oldLoc;
        }

        bool makeDirty(){
            if(! _dirty){
                verify(! obj()["_id"].eoo());
                verify(! _bucket.isNull());
                verify(_pos >= 0);

                if(_id.isEmpty()){
                    _id = obj()["_id"].wrap("").getOwned();
                }
                _o = BSONObj();
                _key = _key.getOwned();
                _pt = _pt.getOwned();
                _dirty = true;

                return true;
            }

            return false;
        }

        BSONObj _key;
        DiskLoc _loc;
        BSONObj _o;
        BSONObj _pt;

        double _distance;
        bool _exact;

        BSONObj _id;
        bool _dirty;
        DiskLoc _bucket;
        int _pos;
    };

    // GeoBrowse subclasses this
    class GeoAccumulator {
    public:
        GeoAccumulator(TwoDAccessMethod* accessMethod, const BSONObj& filter, bool uniqueDocs,
                       bool needDistance)
            : _accessMethod(accessMethod), _converter(accessMethod->getParams().geoHashConverter),
              _lookedAt(0), _matchesPerfd(0), _objectsLoaded(0), _pointsLoaded(0), _found(0),
              _uniqueDocs(uniqueDocs), _needDistance(needDistance) {

            if (! filter.isEmpty()) {
                _matcher.reset(new CoveredIndexMatcher(filter,
                    accessMethod->getDescriptor()->keyPattern()));
                GEODEBUG("Matcher is now " << _matcher->docMatcher().toString());
            }
        }

        virtual ~GeoAccumulator() { }
        enum KeyResult { BAD, BORDER, GOOD };

        virtual void add(const GeoKeyNode& node) {
            GEODEBUG("\t\t\t\t checking key " << node._key.toString())

            _lookedAt++;

            // Approximate distance check using key data
            double keyD = 0;
            Point keyP(_converter->unhashToPoint(node._key.firstElement()));
            KeyResult keyOk = approxKeyCheck(keyP, keyD);
            if (keyOk == BAD) {
                GEODEBUG("\t\t\t\t bad distance : " << node.recordLoc.obj()  << "\t" << keyD);
                return;
            }
            GEODEBUG("\t\t\t\t good distance : " << node.recordLoc.obj()  << "\t" << keyD);

            // Check for match using other key (and potentially doc) criteria
            // Remember match results for each object
            map<DiskLoc, bool>::iterator match = _matched.find(node.recordLoc);
            bool newDoc = match == _matched.end();
            if(newDoc) {
                GEODEBUG("\t\t\t\t matching new doc with "
                         << (_matcher ? _matcher->docMatcher().toString() : "(empty)"));

                // matcher
                MatchDetails details;
                if (_matcher.get()) {
                    bool good = _matcher->matchesWithSingleKeyIndex(node._key, node.recordLoc,
                                                                    &details);
                    _matchesPerfd++;

                    if (details.hasLoadedRecord())
                        _objectsLoaded++;

                    if (! good) {
                        GEODEBUG("\t\t\t\t didn't match : " << node.recordLoc.obj()["_id"]);
                        _matched[ node.recordLoc ] = false;
                        return;
                    }
                }

                _matched[ node.recordLoc ] = true;

                if (! details.hasLoadedRecord()) // don't double count
                    _objectsLoaded++;

            } else if(!((*match).second)) {
                GEODEBUG("\t\t\t\t previously didn't match : " << node.recordLoc.obj()["_id"]);
                return;
            }

            // Exact check with particular data fields
            // Can add multiple points
            int diff = addSpecific(node, keyP, keyOk == BORDER, keyD, newDoc);
            //int diff = addSpecific(node, keyP, keyOk == BORDER, keyD);
            if(diff > 0) _found += diff;
            else _found -= -diff;
        }

        virtual void getPointsFor(const BSONObj& key, const BSONObj& obj,
                                  vector<BSONObj> &locsForNode, bool allPoints = false) {
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

        virtual int addSpecific(const GeoKeyNode& node, const Point& p, bool inBounds, double d,
                                bool newDoc) = 0;
        virtual KeyResult approxKeyCheck(const Point& p, double& keyD) = 0;
        virtual bool exactDocCheck(const Point& p, double& d) = 0;
        virtual bool expensiveExactCheck(){ return false; }

        long long found() const { return _found; }

        TwoDAccessMethod* _accessMethod;
        shared_ptr<GeoHashConverter> _converter;
        map<DiskLoc, bool> _matched;
        shared_ptr<CoveredIndexMatcher> _matcher;

        long long _lookedAt;
        long long _matchesPerfd;
        long long _objectsLoaded;
        long long _pointsLoaded;
        long long _found;

        bool _uniqueDocs;
        bool _needDistance;
    };

    struct BtreeLocation {
        BtreeLocation() { }

        scoped_ptr<BtreeCursor> _cursor;
        scoped_ptr<FieldRangeSet> _frs;
        // TODO: Turn into a KeyPattern object when FieldRangeVector takes one.
        BSONObj _keyPattern;

        BSONObj key() { return _cursor->currKey(); }

        bool hasPrefix(const GeoHash& hash) {
            BSONObj k = key();
            BSONElement e = k.firstElement();
            if (e.eoo())
                return false;
            return GeoHash(e).hasPrefix(hash);
        }

        bool checkAndAdvance(const GeoHash& hash, int& totalFound, GeoAccumulator* all){
            if(! _cursor->ok() || ! hasPrefix(hash)) return false;

            if(all){
                totalFound++;
                GeoKeyNode n(_cursor->getBucket(), _cursor->getKeyOfs(), _cursor->currLoc(),
                             _cursor->currKey());
                all->add(n);
            }
            _cursor->advance();

            return true;
        }

        void save(){ _cursor->noteLocation(); }
        void restore(){ _cursor->checkLocation(); }

        string toString() {
            stringstream ss;
            ss << "bucket: " << _cursor->getBucket().toString() << " pos: " << _cursor->getKeyOfs()
               << (_cursor->ok() ? (str::stream() << " k: " << _cursor->currKey()
                                    << " o : " << _cursor->current()["_id"])
                                 : (string)"[none]") << endl;
            return ss.str();
        }

        // Returns the min and max keys which bound a particular location.
        // The only time these may be equal is when we actually equal the location
        // itself, otherwise our expanding algorithm will fail.
        static bool initial(IndexDescriptor* descriptor, const TwoDIndexingParams& params,
                             BtreeLocation& min, BtreeLocation& max,
                             GeoHash start, int& found, GeoAccumulator* hopper) {
            verify(descriptor);
            verify(hopper);
            // Would be nice to build this directly, but bug in max/min queries SERVER-3766 and lack
            // of interface makes this easiest for now.

            BSONObj minQuery = BSON(params.geo << BSON("$gt" << MINKEY
                << start.wrap("$lte").firstElement()));
            BSONObj maxQuery = BSON(params.geo << BSON("$lt" << MAXKEY
                << start.wrap("$gt").firstElement()));

            min._frs.reset(new FieldRangeSet(descriptor->parentNS().c_str(),
                                             minQuery, true, false));

            max._frs.reset(new FieldRangeSet(descriptor->parentNS().c_str(),
                                             maxQuery, true, false));

            BSONObjBuilder bob;
            bob.append(params.geo, 1);
            for(vector<pair<string, int> >::const_iterator i = params.other.begin();
                i != params.other.end(); i++){
                bob.append(i->first, i->second);
            }
            BSONObj iSpec = bob.obj();

            min._keyPattern = iSpec;
            max._keyPattern = iSpec;

            shared_ptr<FieldRangeVector> frvMin(new FieldRangeVector(*min._frs, min._keyPattern, -1));
            shared_ptr<FieldRangeVector> frvMax(new FieldRangeVector(*max._frs, max._keyPattern, 1));

            min._cursor.reset(BtreeCursor::make(nsdetails(descriptor->parentNS()),
                                                descriptor->getOnDisk(), frvMin, 0, -1));

            max._cursor.reset(BtreeCursor::make(nsdetails(descriptor->parentNS()),
                                                descriptor->getOnDisk(), frvMax, 0, 1));

            return min._cursor->ok() || max._cursor->ok();
        }
    };

    class GeoCursorBase {
    public:
        virtual ~GeoCursorBase() { }
        virtual void explainDetails(BSONObjBuilder& b) { }
        virtual bool ok() = 0;
        bool eof() { return !ok(); }
        virtual BSONObj current() = 0;
        virtual DiskLoc currLoc() = 0;
        virtual bool advance() = 0; /*true=ok*/
        virtual BSONObj currKey() const = 0;
        static const shared_ptr<CoveredIndexMatcher> otherEmptyMatcher;
        virtual void noteLocation() { }
        virtual void checkLocation() { }
        virtual bool supportGetMore() { return false; }
        virtual bool supportYields() { return false; }
        virtual bool getsetdup(DiskLoc loc) { return false; }
        virtual bool modifiedKeys() const { return true; }
        virtual bool isMultiKey() const { return false; }
        virtual bool autoDedup() const { return false; }
        virtual string toString() = 0;
    };

    const shared_ptr<CoveredIndexMatcher> GeoCursorBase::otherEmptyMatcher(
        new CoveredIndexMatcher(BSONObj(), BSONObj()));

    // TODO: Pull out the cursor bit from the browse, have GeoBrowse as field of cursor to clean up
    // this hierarchy a bit.  Also probably useful to look at whether GeoAccumulator can be a member
    // instead of a superclass.
    class GeoBrowse : public GeoCursorBase, public GeoAccumulator {
    public:
        // The max points which should be added to an expanding box at one time
        static const int maxPointsHeuristic = 50;

        // Expand states
        enum State {
            START,
            DOING_EXPAND,
            DONE_NEIGHBOR,
            DONE
        } _state;

        GeoBrowse(TwoDAccessMethod* accessMethod, string type, BSONObj filter = BSONObj(),
                  bool uniqueDocs = true, bool needDistance = false)
            : GeoCursorBase(),
              GeoAccumulator(accessMethod, filter, uniqueDocs, needDistance),
              _type(type), _filter(filter), _firstCall(true), _noted(false), _nscanned(),
              _nDirtied(0), _nChangedOnYield(0), _nRemovedOnYield(0), _centerPrefix(0, 0, 0),
              _btreeInterface(accessMethod->getInterface()),
              _descriptor(accessMethod->getDescriptor()),
              _converter(accessMethod->getParams().geoHashConverter),
              _params(accessMethod->getParams()) {

            // Set up the initial expand state
            _state = START;
            _neighbor = -1;
            _foundInExp = 0;

        }

        virtual string toString() { return (string)"GeoBrowse-" + _type; }

        virtual bool ok() {
            bool filled = false;
            LOG(CDEBUG) << "Checking cursor, in state " << (int) _state << ", first call "
                        << _firstCall << ", empty : " << _cur.isEmpty() << ", dirty : "
                        << _cur.isDirty() << ", stack : " << _stack.size() << endl;

            bool first = _firstCall;
            if (_firstCall) {
                fillStack(maxPointsHeuristic);
                filled = true;
                _firstCall = false;
            }
            if (! _cur.isCleanAndEmpty() || _stack.size()) {
                if (first) { ++_nscanned; }
                if(_noted && filled) noteLocation();
                return true;
            }

            while (moreToDo()) {
                LOG(CDEBUG) << "Refilling stack..." << endl;
                fillStack(maxPointsHeuristic);
                filled = true;
                if (! _cur.isCleanAndEmpty()) {
                    if (first) { ++_nscanned; }
                    if(_noted && filled) noteLocation();
                    return true;
                }
            }

            if(_noted && filled) noteLocation();
            return false;
        }

        virtual bool advance() {
            _cur._o = BSONObj();

            if (_stack.size()) {
                _cur = _stack.front();
                _stack.pop_front();
                ++_nscanned;
                return true;
            }

            if (! moreToDo()) return false;

            bool filled = false;
            while (_cur.isCleanAndEmpty() && moreToDo()){
                fillStack(maxPointsHeuristic);
                filled = true;
            }

            if(_noted && filled) noteLocation();
            return ! _cur.isCleanAndEmpty() && ++_nscanned;
        }

        virtual void noteLocation() {
            _noted = true;
            LOG(CDEBUG) << "Noting location with " << _stack.size()
                        << (_cur.isEmpty() ? "" : " + 1 ") << " points " << endl;

            // Make sure we advance past the point we're at now,
            // since the current location may move on an update/delete
            // if(_state == DOING_EXPAND){
            //     if(_min.hasPrefix(_prefix)){ _min.advance(-1, _foundInExp, this); }
            //    if(_max.hasPrefix(_prefix)){ _max.advance( 1, _foundInExp, this); }
            // }

            // Remember where our _max, _min are
            _min.save();
            _max.save();

            LOG(CDEBUG) << "Min " << _min.toString() << endl;
            LOG(CDEBUG) << "Max " << _max.toString() << endl;

            // Dirty all our queued stuff
            for(list<GeoPoint>::iterator i = _stack.begin(); i != _stack.end(); i++){
                LOG(CDEBUG) << "Undirtying stack point with id " << i->_id << endl;
                if(i->makeDirty()) _nDirtied++;
                verify(i->isDirty());
            }

            // Check current item
            if(! _cur.isEmpty()){
                if(_cur.makeDirty()) _nDirtied++;
            }

            // Our cached matches become invalid now
            //_matched.clear();
        }

        /*
        void fixMatches(DiskLoc oldLoc, DiskLoc newLoc){
            map<DiskLoc, bool>::iterator match = _matched.find(oldLoc);
            if(match != _matched.end()){
                bool val = match->second;
                _matched.erase(oldLoc);
                _matched[ newLoc ] = val;
            }
        }*/

        /* called before query getmore block is iterated */
        virtual void checkLocation() {
            LOG(CDEBUG) << "Restoring location with " << _stack.size()
                        << (! _cur.isDirty() ? "" : " + 1 ") << " points " << endl;
            // We can assume an error was thrown earlier if this database somehow disappears
            // Recall our _max, _min
            _min.restore();
            _max.restore();

            LOG(CDEBUG) << "Min " << _min.toString() << endl;
            LOG(CDEBUG) << "Max " << _max.toString() << endl;

            // If the current key moved, we may have been advanced past the current point
            // - need to check this
            // if(_state == DOING_EXPAND){
            //    if(_min.hasPrefix(_prefix)){ _min.advance(-1, _foundInExp, this); }
            //    if(_max.hasPrefix(_prefix)){ _max.advance( 1, _foundInExp, this); }
            //}

            // Undirty all the queued stuff
            // Dirty all our queued stuff
            list<GeoPoint>::iterator i = _stack.begin();
            while(i != _stack.end()){
                LOG(CDEBUG) << "Undirtying stack point with id " << i->_id << endl;

                DiskLoc oldLoc;
                if(i->unDirty(_btreeInterface, _descriptor, oldLoc)){
                    // Document is in same location
                    LOG(CDEBUG) << "Undirtied " << oldLoc << endl;
                    i++;
                } else if(! i->loc().isNull()){
                    // Re-found document somewhere else
                    LOG(CDEBUG) << "Changed location of " << i->_id << " : "
                                << i->loc() << " vs " << oldLoc << endl;
                    _nChangedOnYield++;
                    //fixMatches(oldLoc, i->loc());
                    i++;
                } else {
                    // Can't re-find document
                    LOG(CDEBUG) << "Removing document " << i->_id << endl;
                    _nRemovedOnYield++;
                    _found--;
                    verify(_found >= 0);
                    // Can't find our key again, remove
                    i = _stack.erase(i);
                }
            }

            if(_cur.isDirty()){
                LOG(CDEBUG) << "Undirtying cur point with id : " << _cur._id << endl;
            }

            // Check current item
            DiskLoc oldLoc;
            if(_cur.isDirty() && ! _cur.unDirty(_btreeInterface, _descriptor, oldLoc)){
                if(_cur.loc().isNull()){
                    // Document disappeared!
                    LOG(CDEBUG) << "Removing cur point " << _cur._id << endl;
                    _nRemovedOnYield++;
                    advance();
                } else{
                    // Document moved
                    LOG(CDEBUG) << "Changed location of cur point " << _cur._id << " : "
                                << _cur.loc() << " vs " << oldLoc << endl;
                    _nChangedOnYield++;
                    //fixMatches(oldLoc, _cur.loc());
                }
            }

            _noted = false;
        }

        virtual Record* _current() { verify(ok()); LOG(CDEBUG + 1) << "_current " << _cur._loc.obj()["_id"] << endl; return _cur._loc.rec(); }
        virtual BSONObj current() { verify(ok()); LOG(CDEBUG + 1) << "current " << _cur._o << endl; return _cur._o; }
        virtual DiskLoc currLoc() { verify(ok()); LOG(CDEBUG + 1) << "currLoc " << _cur._loc << endl; return _cur._loc; }
        virtual BSONObj currKey() const { return _cur._key; }

        virtual CoveredIndexMatcher* matcher() const {
            if(_matcher.get()) return _matcher.get();
            else return GeoCursorBase::otherEmptyMatcher.get();
        }

        // Are we finished getting points?
        virtual bool moreToDo() { return _state != DONE; }
        virtual bool supportGetMore() { return true; }

        Box makeBox(const GeoHash &hash) const {
            double sizeEdge = _converter->sizeEdge(hash);
            Point min(_converter->unhashToPoint(hash));
            Point max(min.x + sizeEdge, min.y + sizeEdge);
            return Box(min, max);
        }

        // Fills the stack, but only checks a maximum number of maxToCheck points at a time.
        // Further calls to this function will continue the expand/check neighbors algorithm.
        virtual void fillStack(int maxToCheck, int maxToAdd = -1, bool onlyExpand = false) {
#ifdef GEODEBUGGING
            log() << "Filling stack with maximum of " << maxToCheck
                  << ", state : " << (int) _state << endl;
#endif
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
                GEODEBUG("initializing btree");

#ifdef GEODEBUGGING
                log() << "Initializing from b-tree with hash of " << _prefix << " @ "
                      << Box(_g, _prefix) << endl;
#endif

                if (!BtreeLocation::initial(_descriptor, _params, _min, _max, _prefix,
                                            _foundInExp, this)) {
                    _state = isNeighbor ? DONE_NEIGHBOR : DONE;
                } else {
                    _state = DOING_EXPAND;
                    _lastPrefix.reset();
                }

                GEODEBUG((_state == DONE_NEIGHBOR || _state == DONE ? "not initialized"
                                                                    : "initializedFig"));
            }

            // Doing the actual box expansion
            if (_state == DOING_EXPAND) {
                while (true) {
                    GEODEBUG("box prefix [" << _prefix << "]");
#ifdef GEODEBUGGING
                    if(_prefix.constrains()) {
                        log() << "current expand box : " << Box(_g, _prefix).toString() << endl;
                    }
                    else {
                        log() << "max expand box." << endl;
                    }
#endif
                    GEODEBUG("expanding box points... ");

                    // Record the prefix we're actively exploring...
                    _expPrefix.reset(new GeoHash(_prefix));

                    // Find points inside this prefix
                    while (_min.checkAndAdvance(_prefix, _foundInExp, this)
                           && _foundInExp < maxFound && _found < maxAdded) {}
                    while (_max.checkAndAdvance(_prefix, _foundInExp, this)
                           && _foundInExp < maxFound && _found < maxAdded) {}

#ifdef GEODEBUGGING
                    log() << "finished expand, checked : "
                          << (maxToCheck - (maxFound - _foundInExp))
                          << " found : " << (maxToAdd - (maxAdded - _found))
                          << " max : " << maxToCheck << " / " << maxToAdd << endl;
#endif
                    GEODEBUG("finished expand, found : " << (maxToAdd - (maxAdded - _found)));
                    if(_foundInExp >= maxFound || _found >= maxAdded) return;

                    // We've searched this prefix fully, remember
                    _lastPrefix.reset(new GeoHash(_prefix));

                    // If we've searched the entire space, we're finished.
                    if (! _prefix.constrains()) {
                        GEODEBUG("box exhausted");
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

                    GEODEBUG("moving to neighbor " << _neighbor << " @ " << i << ", " << j
                                                   << " fringe : " << _fringe.size());
                    PREFIXDEBUG(_centerPrefix, _g);
                    PREFIXDEBUG(_neighborPrefix, _g);

                    while(_fringe.size() > 0) {
                        _prefix = _neighborPrefix + _fringe.back();
                        Box cur(makeBox(_prefix));

                        PREFIXDEBUG(_prefix, _g);

                        double intAmt = intersectsBox(cur);

                        // No intersection
                        if(intAmt <= 0) {
                            GEODEBUG("skipping box" << cur.toString());
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

        // The initial geo hash box for our first expansion
        virtual GeoHash expandStartHash() = 0;

        // Whether the current box width is big enough for our search area
        virtual bool fitsInBox(double width) = 0;

        // The amount the current box overlaps our search area
        virtual double intersectsBox(Box& cur) = 0;

        bool remembered(BSONObj o){
            BSONObj seenId = o["_id"].wrap("").getOwned();
            if(_seenIds.find(seenId) != _seenIds.end()){
                LOG(CDEBUG + 1) << "Object " << o["_id"] << " already seen." << endl;
                return true;
            } else{
                _seenIds.insert(seenId);
                LOG(CDEBUG + 1) << "Object " << o["_id"] << " remembered." << endl;
                return false;
            }
        }

        virtual int addSpecific(const GeoKeyNode& node, const Point& keyP, bool onBounds,
                                double keyD, bool potentiallyNewDoc) {
            int found = 0;
            // We need to handle every possible point in this method, even those not in the key
            // value, to avoid us tracking which hashes we've already seen.
            if(! potentiallyNewDoc){ return 0; }

            // Final check for new doc
            // OK to touch, since we're probably returning this object now
            if(remembered(node.recordLoc.obj())) return 0;

            if(_uniqueDocs && ! onBounds) {
                //log() << "Added ind to " << _type << endl;
                _stack.push_front(GeoPoint(node));
                found++;
            } else {
                // We now handle every possible point in the document, even those not in the key
                // value, since we're iterating through them anyway - prevents us from having to
                // save the hashes we've seen per-doc
                // If we're filtering by hash, get the original
                bool expensiveExact = expensiveExactCheck();

                vector< BSONObj > locs;
                getPointsFor(node._key, node.recordLoc.obj(), locs, true);
                for(vector< BSONObj >::iterator i = locs.begin(); i != locs.end(); ++i){
                    double d = -1;
                    Point p(*i);
                    // We can avoid exact document checks by redoing approx checks,
                    // if the exact checks are more expensive.
                    bool needExact = true;
                    if(expensiveExact){
                        verify(false);
                        KeyResult result = approxKeyCheck(p, d);
                        if(result == BAD) continue;
                        else if(result == GOOD) needExact = false;
                    }

                    if(! needExact || exactDocCheck(p, d)){
                        //log() << "Added mult to " << _type << endl;
                        _stack.push_front(GeoPoint(node));
                        found++;
                        // If returning unique, just exit after first point is added
                        if(_uniqueDocs) break;
                    }
                }
            }

            while(_cur.isCleanAndEmpty() && _stack.size() > 0){
                _cur = _stack.front();
                _stack.pop_front();
            }

            return found;
        }

        virtual long long nscanned() {
            if (_firstCall) { ok(); }
            return _nscanned;
        }

        virtual void explainDetails(BSONObjBuilder& b){
            b << "lookedAt" << _lookedAt;
            b << "matchesPerfd" << _matchesPerfd;
            b << "objectsLoaded" << _objectsLoaded;
            b << "pointsLoaded" << _pointsLoaded;
            b << "pointsSavedForYield" << _nDirtied;
            b << "pointsChangedOnYield" << _nChangedOnYield;
            b << "pointsRemovedOnYield" << _nRemovedOnYield;
        }

        virtual BSONObj prettyIndexBounds() const {
            vector<GeoHash>::const_iterator i = _expPrefixes.end();
            if(_expPrefixes.size() > 0 && *(--i) != *(_expPrefix.get()))
                _expPrefixes.push_back(*(_expPrefix.get()));

            BSONObjBuilder bob;
            BSONArrayBuilder bab;
            for(i = _expPrefixes.begin(); i != _expPrefixes.end(); ++i){
                bab << makeBox(*i).toBSON();
            }
            bob << _params.geo << bab.arr();
            return bob.obj();
        }

        void notePrefix() { _expPrefixes.push_back(_prefix); }

        string _type;
        BSONObj _filter;
        list<GeoPoint> _stack;
        set<BSONObj> _seenIds;

        GeoPoint _cur;
        bool _firstCall;
        bool _noted;

        long long _nscanned;
        long long _nDirtied;
        long long _nChangedOnYield;
        long long _nRemovedOnYield;

        // The current box we're expanding (-1 is first/center box)
        int _neighbor;

        // The points we've found so far
        int _foundInExp;

        // The current hash prefix we're expanding and the center-box hash prefix
        GeoHash _prefix;
        shared_ptr<GeoHash> _lastPrefix;
        GeoHash _centerPrefix;
        list<string> _fringe;
        int recurseDepth;
        Box _centerBox;

        // Start and end of our search range in the current box
        BtreeLocation _min;
        BtreeLocation _max;

        shared_ptr<GeoHash> _expPrefix;
        mutable vector<GeoHash> _expPrefixes;
        BtreeInterface* _btreeInterface;
        IndexDescriptor* _descriptor;
        shared_ptr<GeoHashConverter> _converter;
        TwoDIndexingParams _params;
    };

    class GeoHopper : public GeoBrowse {
    public:
        typedef multiset<GeoPoint> Holder;

        GeoHopper(TwoDAccessMethod* accessMethod,
                  unsigned max,
                  const Point& n,
                  const BSONObj& filter = BSONObj(),
                  double maxDistance = numeric_limits<double>::max(),
                  GeoDistType type = GEO_PLANE,
                  bool uniqueDocs = false,
                  bool needDistance = true)
            : GeoBrowse(accessMethod, "search", filter, uniqueDocs, needDistance),
              _max(max),
              _near(n),
              _maxDistance(maxDistance),
              _type(type),
              _distError(type == GEO_PLANE
                ? accessMethod->getParams().geoHashConverter->getError()
                : accessMethod->getParams().geoHashConverter->getErrorSphere()),
              _farthest(0) { }

        virtual KeyResult approxKeyCheck(const Point& p, double& d) {
            // Always check approximate distance, since it lets us avoid doing
            // checks of the rest of the object if it succeeds
            switch (_type) {
            case GEO_PLANE:
                d = distance(_near, p);
                break;
            case GEO_SPHERE:
                checkEarthBounds(p);
                d = spheredist_deg(_near, p);
                break;
            default: verify(false);
            }
            verify(d >= 0);

            GEODEBUG("\t\t\t\t\t\t\t checkDistance " << _near.toString()
                      << "\t" << p.toString() << "\t" << d
                      << " farthest: " << farthest());

            // If we need more points
            double borderDist = (_points.size() < _max ? _maxDistance : farthest());

            if (d >= borderDist - 2 * _distError && d <= borderDist + 2 * _distError) return BORDER;
            else return d < borderDist ? GOOD : BAD;
        }

        virtual bool exactDocCheck(const Point& p, double& d){
            bool within = false;

            // Get the appropriate distance for the type
            switch (_type) {
            case GEO_PLANE:
                d = distance(_near, p);
                within = distanceWithin(_near, p, _maxDistance);
                break;
            case GEO_SPHERE:
                checkEarthBounds(p);
                d = spheredist_deg(_near, p);
                within = (d <= _maxDistance);
                break;
            default: verify(false);
            }

            return within;
        }

        // Always in distance units, whether radians or normal
        double farthest() const { return _farthest; }

        virtual int addSpecific(const GeoKeyNode& node, const Point& keyP, bool onBounds,
                                double keyD, bool potentiallyNewDoc) {
            // Unique documents
            GeoPoint newPoint(node, keyD, false);
            int prevSize = _points.size();

            // STEP 1 : Remove old duplicate points from the set if needed
            if(_uniqueDocs){
                // Lookup old point with same doc
                map<DiskLoc, Holder::iterator>::iterator oldPointIt = _seenPts.find(newPoint.loc());

                if(oldPointIt != _seenPts.end()){
                    const GeoPoint& oldPoint = *(oldPointIt->second);
                    // We don't need to care if we've already seen this same approx pt or better,
                    // or we've already gone to disk once for the point
                    if(oldPoint < newPoint){
                        GEODEBUG("\t\tOld point closer than new point");
                        return 0;
                    }
                    GEODEBUG("\t\tErasing old point " << oldPointIt->first.obj());
                    _points.erase(oldPointIt->second);
                }
            }

            Holder::iterator newIt = _points.insert(newPoint);
            if(_uniqueDocs) _seenPts[ newPoint.loc() ] = newIt;

            GEODEBUG("\t\tInserted new point " << newPoint.toString() << " approx : " << keyD);

            verify(_max > 0);

            Holder::iterator lastPtIt = _points.end();
            lastPtIt--;
            _farthest = lastPtIt->distance() + 2 * _distError;
            return _points.size() - prevSize;
        }

        // Removes extra points from end of _points set.
        // Check can be a bit costly if we have lots of exact points near borders,
        // so we'll do this every once and awhile.
        void processExtraPoints(){
            if(_points.size() == 0) return;
            int prevSize = _points.size();

            // Erase all points from the set with a position >= _max *and*
            // whose distance isn't close to the _max - 1 position distance
            int numToErase = _points.size() - _max;
            if(numToErase < 0) numToErase = 0;

            // Get the first point definitely in the _points array
            Holder::iterator startErase = _points.end();
            for(int i = 0; i < numToErase + 1; i++) startErase--;
            _farthest = startErase->distance() + 2 * _distError;

            startErase++;
            while(numToErase > 0 && startErase->distance() <= _farthest){
                GEODEBUG("\t\tNot erasing point " << startErase->toString());
                numToErase--;
                startErase++;
                verify(startErase != _points.end() || numToErase == 0);
            }

            if(_uniqueDocs){
                for(Holder::iterator i = startErase; i != _points.end(); ++i)
                    _seenPts.erase(i->loc());
            }

            _points.erase(startErase, _points.end());

            int diff = _points.size() - prevSize;
            if(diff > 0) _found += diff;
            else _found -= -diff;
        }

        unsigned _max;
        Point _near;
        Holder _points;
        double _maxDistance;
        GeoDistType _type;
        double _distError;
        double _farthest;

        // Safe to use currently since we don't yield in $near searches.  If we do start to yield,
        // we may need to replace dirtied disklocs in our holder / ensure our logic is correct.
        map<DiskLoc, Holder::iterator> _seenPts;
    };

    class GeoSearch : public GeoHopper {
    public:
        GeoSearch(TwoDAccessMethod* accessMethod,
                  const Point& startPt,
                  int numWanted = 100,
                  BSONObj filter = BSONObj(),
                  double maxDistance = numeric_limits<double>::max(),
                  GeoDistType type = GEO_PLANE,
                  bool uniqueDocs = false,
                  bool needDistance = false)
           : GeoHopper(accessMethod, numWanted, startPt, filter, maxDistance, type,
                       uniqueDocs, needDistance),
             _start(accessMethod->getParams().geoHashConverter->hash(startPt.x, startPt.y)),
             _numWanted(numWanted),
             _type(type),
             _params(accessMethod->getParams()) {

            _nscanned = 0;
            _found = 0;

            if(_maxDistance < 0){
               _scanDistance = numeric_limits<double>::max();
            } else if (type == GEO_PLANE) {
                _scanDistance = maxDistance + _params.geoHashConverter->getError();
            } else if (type == GEO_SPHERE) {
                checkEarthBounds(startPt);
                // TODO: consider splitting into x and y scan distances
                _scanDistance = computeXScanDistance(startPt.y,
                    rad2deg(_maxDistance) + _params.geoHashConverter->getError());
            }

            verify(_scanDistance > 0);
        }


    /** Check if we've already looked at a key.  ALSO marks as seen, anticipating a follow-up
     * call to add().  This is broken out to avoid some work extracting the key bson if it's an
     * already seen point.
     */
    private:
        set< pair<DiskLoc,int> > _seen;
    public:
        void exec() {
            if(_numWanted == 0) return;

            /*
             * Search algorithm
             * 1) use geohash prefix to find X items
             * 2) compute max distance from want to an item
             * 3) find optimal set of boxes that complete circle
             * 4) use regular btree cursors to scan those boxes
             */

           // Part 1
           {
               do {
                   long long f = found();
                   verify(f <= 0x7fffffff);
                   fillStack(maxPointsHeuristic, _numWanted - static_cast<int>(f), true);
                   processExtraPoints();
               } while(_state != DONE && _state != DONE_NEIGHBOR &&
                        found() < _numWanted &&
                        (!_prefix.constrains() ||
                         _params.geoHashConverter->sizeEdge(_prefix) <= _scanDistance));

               // If we couldn't scan or scanned everything, we're done
               if(_state == DONE){
                   expandEndPoints();
                   return;
               }
           }

#ifdef GEODEBUGGING
           log() << "part 1 of near search completed, found " << found()
                 << " points (out of " << _foundInExp << " scanned)"
                 << " in expanded region " << _prefix << " @ " << Box(_g, _prefix)
                 << " with furthest distance " << farthest() << endl;
#endif

           // Part 2
            {
               // Find farthest distance for completion scan
                double farDist = farthest();
                if(found() < _numWanted) {
                    // Not enough found in Phase 1
                    farDist = _scanDistance;
                }
                else if (_type == GEO_PLANE) {
                   // Enough found, but need to search neighbor boxes
                    farDist += _params.geoHashConverter->getError();
                }
                else if (_type == GEO_SPHERE) {
                   // Enough found, but need to search neighbor boxes
                    farDist = std::min(_scanDistance,
                                       computeXScanDistance(_near.y,
                                         rad2deg(farDist))
                                         + 2 * _params.geoHashConverter->getError());
                }
                verify(farDist >= 0);
                GEODEBUGPRINT(farDist);

                // Find the box that includes all the points we need to return
                _want = Box(_near.x - farDist, _near.y - farDist, farDist * 2);
                GEODEBUGPRINT(_want.toString());

                // Remember the far distance for further scans
                _scanDistance = farDist;

                // Reset the search, our distances have probably changed
                if(_state == DONE_NEIGHBOR){
                   _state = DOING_EXPAND;
                   _neighbor = -1;
                }

                // Do regular search in the full region
                do {
                   fillStack(maxPointsHeuristic);
                   processExtraPoints();
                }
                while(_state != DONE);
            }

            GEODEBUG("done near search with " << _points.size() << " points ");
            expandEndPoints();
        }

        void addExactPoints(const GeoPoint& pt, Holder& points, bool force){
            int before, after;
            addExactPoints(pt, points, before, after, force);
        }

        void addExactPoints(const GeoPoint& pt, Holder& points, int& before, int& after,
                            bool force){
            before = 0;
            after = 0;

            GEODEBUG("Adding exact points for " << pt.toString());

            if(pt.isExact()){
                if(force) points.insert(pt);
                return;
            }

            vector<BSONObj> locs;
            getPointsFor(pt.key(), pt.obj(), locs, _uniqueDocs);

            GeoPoint nearestPt(pt, -1, true);

            for(vector<BSONObj>::iterator i = locs.begin(); i != locs.end(); i++){
                Point loc(*i);
                double d;
                if(! exactDocCheck(loc, d)) continue;

                if(_uniqueDocs && (nearestPt.distance() < 0 || d < nearestPt.distance())){
                    nearestPt._distance = d;
                    nearestPt._pt = *i;
                    continue;
                } else if(! _uniqueDocs){
                    GeoPoint exactPt(pt, d, true);
                    exactPt._pt = *i;
                    points.insert(exactPt);
                    exactPt < pt ? before++ : after++;
                }
            }

            if(_uniqueDocs && nearestPt.distance() >= 0){
                points.insert(nearestPt);
                if(nearestPt < pt) before++;
                else after++;
            }
        }

        // TODO: Refactor this back into holder class, allow to run periodically when we are seeing
        // a lot of pts
        void expandEndPoints(bool finish = true){
            processExtraPoints();
            // All points in array *could* be in maxDistance

            // Step 1 : Trim points to max size TODO:  This check will do little for now, but is
            // skeleton for future work in incremental $near
            // searches
            if(_max > 0){
                int numToErase = _points.size() - _max;
                if(numToErase > 0){
                    Holder tested;
                    // Work backward through all points we're not sure belong in the set
                    Holder::iterator maybePointIt = _points.end();
                    maybePointIt--;
                    double approxMin = maybePointIt->distance() - 2 * _distError;

                    // Insert all
                    int erased = 0;
                    while(_points.size() > 0
                          && (maybePointIt->distance() >= approxMin || erased < numToErase)){

                        Holder::iterator current = maybePointIt;
                        if (current != _points.begin())
                            --maybePointIt;

                        addExactPoints(*current, tested, true);
                        _points.erase(current);
                        erased++;

                        if(tested.size())
                            approxMin = tested.begin()->distance() - 2 * _distError;
                    }

                    int numToAddBack = erased - numToErase;
                    verify(numToAddBack >= 0);

                    Holder::iterator testedIt = tested.begin();
                    for(int i = 0; i < numToAddBack && testedIt != tested.end(); i++){
                        _points.insert(*testedIt);
                        testedIt++;
                    }
                }
            }

            // We've now trimmed first set of unneeded points

            GEODEBUG("\t\t Start expanding, num points : " << _points.size() << " max : " << _max);

            // Step 2: iterate through all points and add as needed
            unsigned expandedPoints = 0;
            Holder::iterator it = _points.begin();
            double expandWindowEnd = -1;

            while(it != _points.end()){
                const GeoPoint& currPt = *it;
                // TODO: If one point is exact, maybe not 2 * _distError

                // See if we're in an expand window
                bool inWindow = currPt.distance() <= expandWindowEnd;
                // If we're not, and we're done with points, break
                if(! inWindow && expandedPoints >= _max) break;

                bool expandApprox = !currPt.isExact() &&
                                    (!_uniqueDocs || (finish && _needDistance) || inWindow);

                if (expandApprox) {
                    // Add new point(s). These will only be added in a radius of 2 * _distError
                    // around the current point, so should not affect previously valid points.
                    int before, after;
                    addExactPoints(currPt, _points, before, after, false);
                    expandedPoints += before;

                    if(_max > 0 && expandedPoints < _max)
                        expandWindowEnd = currPt.distance() + 2 * _distError;

                    // Iterate to the next point
                    Holder::iterator current = it++;
                    // Erase the current point
                    _points.erase(current);
                } else{
                    expandedPoints++;
                    it++;
                }
            }

            GEODEBUG("\t\tFinished expanding, num points : " << _points.size()
                     << " max : " << _max);

            // Finish
            // TODO:  Don't really need to trim?
            for(; expandedPoints > _max; expandedPoints--) it--;
            _points.erase(it, _points.end());
        }

        virtual GeoHash expandStartHash(){ return _start; }

        // Whether the current box width is big enough for our search area
        virtual bool fitsInBox(double width){ return width >= _scanDistance; }

        // Whether the current box overlaps our search area
        virtual double intersectsBox(Box& cur){ return cur.intersects(_want); }

        GeoHash _start;
        int _numWanted;
        double _scanDistance;
        long long _nscanned;
        int _found;
        GeoDistType _type;
        Box _want;
        TwoDIndexingParams& _params;
    };

    class GeoSearchCursor : public GeoCursorBase {
    public:
        GeoSearchCursor(shared_ptr<GeoSearch> s)
            : GeoCursorBase(), _s(s), _cur(s->_points.begin()), _end(s->_points.end()),
              _nscanned() {
            if (_cur != _end) {
                ++_nscanned;
            }
        }

        virtual ~GeoSearchCursor() {}

        virtual bool ok() { return _cur != _end; }
        virtual Record* _current() { verify(ok()); return _cur->_loc.rec(); }
        virtual BSONObj current() { verify(ok()); return _cur->_o; }
        virtual DiskLoc currLoc() { verify(ok()); return _cur->_loc; }
        virtual BSONObj currKey() const { return _cur->_key; }
        virtual string toString() { return "GeoSearchCursor"; }
        virtual long long nscanned() { return _nscanned; }

        virtual bool advance() {
            if(ok()){
                _cur++;
                incNscanned();
                return ok();
            }
            return false;
        }

        virtual BSONObj prettyStartKey() const {
            return BSON(_s->_params.geo << _s->_prefix.toString());
        }
        virtual BSONObj prettyEndKey() const {
            GeoHash temp = _s->_prefix;
            temp.move(1, 1);
            return BSON(_s->_params.geo << temp.toString());
        }

        virtual CoveredIndexMatcher* matcher() const {
            if(_s->_matcher.get()) return _s->_matcher.get();
            else return otherEmptyMatcher.get();
        }

        shared_ptr<GeoSearch> _s;
        GeoHopper::Holder::iterator _cur;
        GeoHopper::Holder::iterator _end;

        void incNscanned() { if (ok()) { ++_nscanned; } }
        long long _nscanned;
    };

    class GeoCircleBrowse : public GeoBrowse {
    public:
        GeoCircleBrowse(TwoDAccessMethod* accessMethod, const BSONObj& circle,
                        BSONObj filter = BSONObj(), const string& type = "$center",
                        bool uniqueDocs = true)
            : GeoBrowse(accessMethod, "circle", filter, uniqueDocs) {

            uassert(16783, "$center needs 2 fields (middle,max distance)", circle.nFields() == 2);

            BSONObjIterator i(circle);
            BSONElement center = i.next();

            uassert(16784, "the first field of $center object must be a location object",
                    center.isABSONObj());

            _converter = accessMethod->getParams().geoHashConverter;

            // Get geohash and exact center point
            // TODO: For wrapping search, may be useful to allow center points outside-of-bounds
            // here.  Calculating the nearest point as a hash start inside the region would then be
            // required.
            _start = _converter->hash(center);
            _startPt = Point(center);

            _maxDistance = i.next().numberDouble();
            uassert(16785, "need a max distance >= 0 ", _maxDistance >= 0);

            if (type == "$center") {
                // Look in box with bounds of maxDistance in either direction
                _type = GEO_PLANE;
                xScanDistance = _maxDistance + _converter->getError();
                yScanDistance = _maxDistance + _converter->getError();
            } else if (type == "$centerSphere") {
                // Same, but compute maxDistance using spherical transform
                uassert(16786, "Spherical MaxDistance > PI. Are you sure you are using radians?",
                        _maxDistance < M_PI);
                checkEarthBounds(_startPt);

                _type = GEO_SPHERE;
                // should this be sphere error?
                yScanDistance = rad2deg(_maxDistance) + _converter->getError();
                xScanDistance = computeXScanDistance(_startPt.y, yScanDistance);

                uassert(16787, "Spherical distance would require (unimplemented) wrapping",
                        (_startPt.x + xScanDistance < 180) &&
                        (_startPt.x - xScanDistance > -180) &&
                        (_startPt.y + yScanDistance < 90) &&
                        (_startPt.y - yScanDistance > -90));
            } else {
                uassert(16788, "invalid $center query type: " + type, false);
            }

            // Bounding box includes fudge factor.
            // TODO:  Is this correct, since fudge factor may be spherically transformed?
            _bBox._min = Point(_startPt.x - xScanDistance, _startPt.y - yScanDistance);
            _bBox._max = Point(_startPt.x + xScanDistance, _startPt.y + yScanDistance);

            GEODEBUG("Bounding box for circle query : " << _bBox.toString()
                     << " (max distance : " << _maxDistance << ")"
                     << " starting from " << _startPt.toString());
            ok();
        }

        virtual GeoHash expandStartHash() { return _start; }

        virtual bool fitsInBox(double width) {
            return width >= std::max(xScanDistance, yScanDistance);
        }

        virtual double intersectsBox(Box& cur) {
            return cur.intersects(_bBox);
        }

        virtual KeyResult approxKeyCheck(const Point& p, double& d) {
            // Inexact hash distance checks.
            double error = 0;
            switch (_type) {
            case GEO_PLANE:
                d = distance(_startPt, p);
                error = _converter->getError();
                break;
            case GEO_SPHERE: {
                checkEarthBounds(p);
                d = spheredist_deg(_startPt, p);
                error = _converter->getErrorSphere();
                break;
            }
            default: verify(false);
            }

            // If our distance is in the error bounds...
            if(d >= _maxDistance - error && d <= _maxDistance + error) return BORDER;
            return d > _maxDistance ? BAD : GOOD;
        }

        virtual bool exactDocCheck(const Point& p, double& d){
            switch (_type) {
            case GEO_PLANE: {
                if(distanceWithin(_startPt, p, _maxDistance)) return true;
                break;
            }
            case GEO_SPHERE:
                checkEarthBounds(p);
                if(spheredist_deg(_startPt, p) <= _maxDistance) return true;
                break;
            default: verify(false);
            }

            return false;
        }

        GeoDistType _type;
        GeoHash _start;
        Point _startPt;
        double _maxDistance; // user input
        double xScanDistance; // effected by GeoDistType
        double yScanDistance; // effected by GeoDistType
        Box _bBox;

        shared_ptr<GeoHashConverter> _converter;
    };

    class GeoBoxBrowse : public GeoBrowse {
    public:
        GeoBoxBrowse(TwoDAccessMethod* accessMethod, const BSONObj& box, BSONObj filter = BSONObj(),
                     bool uniqueDocs = true)
            : GeoBrowse(accessMethod, "box", filter, uniqueDocs) {

            _converter = accessMethod->getParams().geoHashConverter;

            uassert(16789, "$box needs 2 fields (bottomLeft,topRight)", box.nFields() == 2);

            // Initialize an *exact* box from the given obj.
            BSONObjIterator i(box);
            _want._min = Point(i.next());
            _want._max = Point(i.next());

            _wantRegion = _want;
            // Need to make sure we're checking regions within error bounds of where we want
            _wantRegion.fudge(_converter->getError());
            fixBox(_wantRegion);
            fixBox(_want);

            uassert(16790, "need an area > 0 ", _want.area() > 0);

            Point center = _want.center();
            _start = _converter->hash(center.x, center.y);

            _fudge = _converter->getError();
            _wantLen = _fudge +
                       std::max((_want._max.x - _want._min.x),
                                 (_want._max.y - _want._min.y)) / 2;

            ok();
        }

        void fixBox(Box& box) {
            if(box._min.x > box._max.x)
                swap(box._min.x, box._max.x);
            if(box._min.y > box._max.y)
                swap(box._min.y, box._max.y);

            double gMin = _converter->getMin();
            double gMax = _converter->getMax();

            if(box._min.x < gMin) box._min.x = gMin;
            if(box._min.y < gMin) box._min.y = gMin;
            if(box._max.x > gMax) box._max.x = gMax;
            if(box._max.y > gMax) box._max.y = gMax;
        }

        void swap(double& a, double& b) {
            double swap = a;
            a = b;
            b = swap;
        }

        virtual GeoHash expandStartHash() {
            return _start;
        }

        virtual bool fitsInBox(double width) {
            return width >= _wantLen;
        }

        virtual double intersectsBox(Box& cur) {
            return cur.intersects(_wantRegion);
        }

        virtual KeyResult approxKeyCheck(const Point& p, double& d) {
            if(_want.onBoundary(p, _fudge)) return BORDER;
            else return _want.inside(p, _fudge) ? GOOD : BAD;

        }

        virtual bool exactDocCheck(const Point& p, double& d){
            return _want.inside(p);
        }

        Box _want;
        Box _wantRegion;
        double _wantLen;
        double _fudge;
        GeoHash _start;
        shared_ptr<GeoHashConverter> _converter;
    };

    class GeoPolygonBrowse : public GeoBrowse {
    public:
        GeoPolygonBrowse(TwoDAccessMethod* accessMethod, const BSONObj& polyPoints,
                         BSONObj filter = BSONObj(), bool uniqueDocs = true)
            : GeoBrowse(accessMethod, "polygon", filter, uniqueDocs) {

            BSONObjIterator i(polyPoints);
            BSONElement first = i.next();
            _poly.add(Point(first));

            while (i.more()) {
                _poly.add(Point(i.next()));
            }

            uassert(16791, "polygon must be defined by three points or more", _poly.size() >= 3);
            _converter = accessMethod->getParams().geoHashConverter;

            _bounds = _poly.bounds();
            // We need to check regions within the error bounds of these bounds
            _bounds.fudge(_converter->getError()); 
            // We don't need to look anywhere outside the space
            _bounds.truncate(_converter->getMin(), _converter->getMax()); 
            _maxDim = _converter->getError() + _bounds.maxDim() / 2;

            ok();
        }

        // The initial geo hash box for our first expansion
        virtual GeoHash expandStartHash() {
            return _converter->hash(_bounds.center());
        }

        // Whether the current box width is big enough for our search area
        virtual bool fitsInBox(double width) {
            return _maxDim <= width;
        }

        // Whether the current box overlaps our search area
        virtual double intersectsBox(Box& cur) {
            return cur.intersects(_bounds);
        }

        virtual KeyResult approxKeyCheck(const Point& p, double& d) {
            int in = _poly.contains(p, _converter->getError());
            if(in == 0) return BORDER;
            else return in > 0 ? GOOD : BAD;
        }

        virtual bool exactDocCheck(const Point& p, double& d){
            return _poly.contains(p);
        }

    private:
        Polygon _poly;
        Box _bounds;
        double _maxDim;
        GeoHash _start;
        shared_ptr<GeoHashConverter> _converter;
    };

    bool TwoDGeoNearRunner::run2DGeoNear(NamespaceDetails* nsd, int idxNo, const BSONObj& cmdObj,
                             const GeoNearArguments &parsedArgs, string& errmsg,
                             BSONObjBuilder& result, unordered_map<string, double>* stats) {

        auto_ptr<IndexDescriptor> descriptor(CatalogHack::getDescriptor(nsd, idxNo));
        auto_ptr<TwoDAccessMethod> sam(new TwoDAccessMethod(descriptor.get()));
        const TwoDIndexingParams& params = sam->getParams();

        uassert(13046, "'near' param missing/invalid", !cmdObj["near"].eoo());
        const Point n(cmdObj["near"]);
        result.append("near", params.geoHashConverter->hash(cmdObj["near"]).toString());

        uassert(16903, "'minDistance' param not supported for 2d index, requires 2dsphere index",
                cmdObj["minDistance"].eoo());

        double maxDistance = numeric_limits<double>::max();
        BSONElement eMaxDistance = cmdObj["maxDistance"];

        if (!eMaxDistance.eoo()) {
            uassert(17085, "maxDistance must be a number", eMaxDistance.isNumber());
            maxDistance = cmdObj["maxDistance"].number();
            uassert(17086, "maxDistance must be non-negative", maxDistance >= 0);
        }

        GeoDistType type = parsedArgs.isSpherical ? GEO_SPHERE : GEO_PLANE;

        GeoSearch gs(sam.get(), n, parsedArgs.numWanted, parsedArgs.query, maxDistance, type,
                     parsedArgs.uniqueDocs, true);

        if (cmdObj["start"].type() == String) {
            GeoHash start ((string) cmdObj["start"].valuestr());
            gs._start = start;
        }

        gs.exec();

        double totalDistance = 0;

        BSONObjBuilder arr(result.subarrayStart("results"));
        int x = 0;
        for (GeoHopper::Holder::iterator i=gs._points.begin(); i!=gs._points.end(); i++) {

            const GeoPoint& p = *i;
            double dis = parsedArgs.distanceMultiplier * p.distance();
            totalDistance += dis;

            BSONObjBuilder bb(arr.subobjStart(BSONObjBuilder::numStr(x++)));
            bb.append("dis", dis);
            if (parsedArgs.includeLocs) {
                if(p._pt.couldBeArray()) bb.append("loc", BSONArray(p._pt));
                else bb.append("loc", p._pt);
            }
            bb.append("obj", p._o);
            bb.done();

            if (arr.len() > BSONObjMaxUserSize) {
                warning() << "Too many results to fit in single document. Truncating..." << endl;
                break;
            }
        }
        arr.done();

        (*stats)["btreelocs"] = gs._nscanned;
        (*stats)["nscanned"] = gs._lookedAt;
        (*stats)["objectsLoaded"] = gs._objectsLoaded;
        (*stats)["avgDistance"] = totalDistance / x;
        (*stats)["maxDistance"] = gs.farthest();

        return true;
    }

    }  // namespace twod_internal

    //
    // IndexCursor below.
    //

    TwoDIndexCursor::TwoDIndexCursor(TwoDAccessMethod* accessMethod)
        : _accessMethod(accessMethod), _numWanted(100) { }

    Status TwoDIndexCursor::setOptions(const CursorOptions& options) {
        _numWanted = options.numWanted;

        if (_numWanted < 0) {
            _numWanted = _numWanted * -1;
        } else if (0 == _numWanted) {
            _numWanted = 100;
        }

        return Status::OK();
    }

    Status TwoDIndexCursor::seek(const BSONObj& position) {
        // false means we want to filter OUT geoFieldsToNuke, not filter to include only that.
        BSONObj filteredQuery = position.filterFieldsUndotted(
            BSON(_accessMethod->getParams().geo << ""), false);

        BSONObjIterator i(position);
        while (i.more()) {
            BSONElement e = i.next();

            if (_accessMethod->getParams().geo != e.fieldName())
                continue;

            if (e.type() == Array) {
                // If we get an array query, assume it is a location, and do a $within { $center :
                // [[x, y], 0] } search
                BSONObj circle = BSON("0" << e.embeddedObjectUserCheck() << "1" << 0);
                _underlyingCursor.reset(new twod_internal::GeoCircleBrowse(_accessMethod, circle, filteredQuery, "$center", true));
            } else if (e.type() == Object) {
                switch (e.embeddedObject().firstElement().getGtLtOp()) {
                case BSONObj::opNEAR: {
                    BSONObj n = e.embeddedObject();
                    e = n.firstElement();
                    twod_internal::GeoDistType type;
                    if (strcmp(e.fieldName(), "$nearSphere") == 0) {
                        type = twod_internal::GEO_SPHERE;
                    } else if ( (strcmp(e.fieldName(), "$near") == 0) || (strcmp(e.fieldName(), "$geoNear") == 0) ) {
                        type = twod_internal::GEO_PLANE;
                    } else {
                        uassert(16792, string("invalid $near search type: ") + e.fieldName(), false);
                        type = twod_internal::GEO_PLANE; // prevents uninitialized warning
                    }

                    uassert(16904,
                        "'$minDistance' param not supported for 2d index, requires 2dsphere index",
                        n["$minDistance"].eoo());

                    double maxDistance = numeric_limits<double>::max();
                    if (e.isABSONObj() && e.embeddedObject().nFields() > 2) {
                        BSONObjIterator i(e.embeddedObject());
                        i.next();
                        i.next();
                        BSONElement e = i.next();
                        if (e.isNumber())
                            maxDistance = e.numberDouble();
                    }
                    {
                        BSONElement e = n["$maxDistance"];
                        if (!e.eoo()) {
                            uassert(17087, "$maxDistance must be a number", e.isNumber());
                            maxDistance = e.numberDouble();
                            uassert(16989, "$maxDistance must be non-negative", maxDistance >= 0);
                            if (twod_internal::GEO_SPHERE == type) {
                                uassert(17088, "$maxDistance too large",
                                        maxDistance <= nextafter(M_PI, DBL_MAX));
                            }
                        }
                    }

                    bool uniqueDocs = false;
                    if(! n["$uniqueDocs"].eoo()) uniqueDocs = n["$uniqueDocs"].trueValue();

                    shared_ptr<twod_internal::GeoSearch> s(
                        new twod_internal::GeoSearch(_accessMethod, Point(e), _numWanted,
                                                     filteredQuery, maxDistance, type, uniqueDocs));
                    s->exec();
                    _underlyingCursor.reset(new twod_internal::GeoSearchCursor(s));
                } break;
                case BSONObj::opWITHIN: {
                    e = e.embeddedObject().firstElement();
                    uassert(16793, "$within has to take an object or array", e.isABSONObj());

                    BSONObj context = e.embeddedObject();
                    e = e.embeddedObject().firstElement();
                    string type = e.fieldName();

                    bool uniqueDocs = true;
                    if (!context["$uniqueDocs"].eoo())
                            uniqueDocs = context["$uniqueDocs"].trueValue();

                    if (startsWith(type,  "$center")) {
                        uassert(16794, "$center has to take an object or array", e.isABSONObj());
                        _underlyingCursor.reset(new twod_internal::GeoCircleBrowse(_accessMethod, e.embeddedObjectUserCheck(), 
                                                                    filteredQuery, type, uniqueDocs));
                    } else if (type == "$box") {
                        uassert(16795, "$box has to take an object or array", e.isABSONObj());
                        _underlyingCursor.reset(new twod_internal::GeoBoxBrowse(_accessMethod, e.embeddedObjectUserCheck(),
                                                                 filteredQuery, uniqueDocs));
                    } else if (startsWith(type, "$poly")) {
                        uassert(16796, "$polygon has to take an object or array", e.isABSONObj());
                        _underlyingCursor.reset(new twod_internal::GeoPolygonBrowse(_accessMethod, e.embeddedObjectUserCheck(),
                                                                     filteredQuery, uniqueDocs));
                    } else {
                        throw UserException(16797, str::stream() << "unknown $within information : "
                                                                 << context
                                                                 << ", a shape must be specified.");
                    }
                } break;
                default:
                    // Otherwise... assume the object defines a point, and we want to do a
                    // zero-radius $within $center

                    _underlyingCursor.reset(new twod_internal::GeoCircleBrowse(_accessMethod,
                        BSON("0" << e.embeddedObjectUserCheck() << "1" << 0), filteredQuery));
                    break;
                }
            }
        }

        if (NULL == _underlyingCursor.get()) {
            throw UserException(16798, (string)"missing geo field ("
                + _accessMethod->getParams().geo + ") in : " + position.toString());
        }
        return Status::OK();
    }

    bool TwoDIndexCursor::isEOF() const { return _underlyingCursor->eof(); }
    BSONObj TwoDIndexCursor::getKey() const { return _underlyingCursor->currKey(); }
    DiskLoc TwoDIndexCursor::getValue() const { return _underlyingCursor->currLoc(); };
    void TwoDIndexCursor::next() { _underlyingCursor->advance(); }
    string TwoDIndexCursor::toString() { return _underlyingCursor->toString(); }
    Status TwoDIndexCursor::savePosition() {
        _underlyingCursor->noteLocation();
        return Status::OK();
    }
    Status TwoDIndexCursor::restorePosition() {
        _underlyingCursor->checkLocation();
        return Status::OK();
    }
    void TwoDIndexCursor::explainDetails(BSONObjBuilder* b) {
        _underlyingCursor->explainDetails(*b);
    }

}  // namespace mongo
