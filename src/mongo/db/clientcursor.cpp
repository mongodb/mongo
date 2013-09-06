/**
 *    Copyright (C) 2008, 2013 10gen Inc.
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

#include "mongo/pch.h"

#include "mongo/db/clientcursor.h"

#include <string>
#include <time.h>
#include <vector>

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/db.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/pagefault.h"
#include "mongo/db/parsed_query.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/repl/write_concern.h"
#include "mongo/db/scanandorder.h"
#include "mongo/platform/random.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/timer.h"

namespace mongo {

    ClientCursor::CCById ClientCursor::clientCursorsById;
    boost::recursive_mutex& ClientCursor::ccmutex( *(new boost::recursive_mutex()) );
    long long ClientCursor::numberTimedOut = 0;
    set<Runner*> ClientCursor::nonCachedRunners;

    void aboutToDeleteForSharding(const StringData& ns,
                                  const Database* db,
                                  const NamespaceDetails* nsd,
                                  const DiskLoc& dl ); // from s/d_logic.h

    ClientCursor::ClientCursor(int qopts, const shared_ptr<Cursor>& c, const string& ns,
                               BSONObj query)
        : _ns(ns), _query(query), _runner(NULL), _c(c), _yieldSometimesTracker(128, 10) {

        _queryOptions = qopts;
        _doingDeletes = false;
        init();
    }

    ClientCursor::ClientCursor(Runner* runner, int qopts, const BSONObj query)
        : _yieldSometimesTracker(128, 10) {

        _runner.reset(runner);
        _ns = runner->ns();
        _query = query;
        _queryOptions = qopts;
        init();
    }

    void ClientCursor::init() {
        _db = cc().database();
        verify( _db );
        verify( _db->ownsNS( _ns ) );

        _idleAgeMillis = 0;
        _leftoverMaxTimeMicros = 0;
        _pinValue = 0;
        _pos = 0;
        
        Lock::assertAtLeastReadLocked(_ns);

        if (_queryOptions & QueryOption_NoCursorTimeout) {
            // cursors normally timeout after an inactivity period to prevent excess memory use
            // setting this prevents timeout of the cursor in question.
            ++_pinValue;
        }

        recursive_scoped_lock lock(ccmutex);
        _cursorid = allocCursorId_inlock();
        clientCursorsById.insert( make_pair(_cursorid, this) );
    }

    ClientCursor::~ClientCursor() {
        if( _pos == -2 ) {
            // defensive: destructor called twice
            wassert(false);
            return;
        }

        {
            recursive_scoped_lock lock(ccmutex);
            if (NULL != _c.get()) {
                // Removes 'this' from bylocation map
                setLastLoc_inlock( DiskLoc() );
            }

            clientCursorsById.erase(_cursorid);

            // defensive:
            _cursorid = INVALID_CURSOR_ID;
            _pos = -2;
            _pinValue = 0;
        }
    }

    // static
    void ClientCursor::assertNoCursors() {
        recursive_scoped_lock lock(ccmutex);
        if (clientCursorsById.size() > 0) {
            log() << "ERROR clientcursors exist but should not at this point" << endl;
            ClientCursor *cc = clientCursorsById.begin()->second;
            log() << "first one: " << cc->_cursorid << ' ' << cc->_ns << endl;
            clientCursorsById.clear();
            verify(false);
        }
    }

    void ClientCursor::invalidate(const char *ns) {
        Lock::assertWriteLocked(ns);
        int len = strlen(ns);
        const char* dot = strchr(ns, '.');
        verify(len > 0 && dot);

        // first (and only) dot is the last char
        bool isDB = (dot == &ns[len-1]);

        Database *db = cc().database();
        verify(db);
        verify(str::startsWith(ns, db->name()));

        // Look at all active non-cached Runners.  These are the runners that are in auto-yield mode
        // that are not attached to the the client cursor. For example, all internal runners don't
        // need to be cached -- there will be no getMore.
        for (set<Runner*>::iterator it = nonCachedRunners.begin(); it != nonCachedRunners.end();
             ++it) {

            Runner* runner = *it;
            const string& runnerNS = runner->ns();
            if ((isDB && str::startsWith(runnerNS, ns)) || (str::equals(runnerNS.c_str(), ns))) {
                runner->kill();
            }
        }

        recursive_scoped_lock cclock(ccmutex);
        CCById::const_iterator it = clientCursorsById.begin();
        while (it != clientCursorsById.end()) {
            ClientCursor* cc = it->second;

            // We're only interested in cursors over one db.
            if (cc->_db != db) {
                ++it;
                continue;
            }

            bool shouldDelete = false;

            // We will only delete CCs with runners that are not actively in use.  The runners that
            // are actively in use are instead kill()-ed.
            if (NULL != cc->_runner.get()) {
                verify(NULL == cc->c());

                // If there is a pinValue >= 100, somebody is actively using the CC and we do not
                // delete it.  Instead we notify the holder that we killed it.  The holder will then
                // delete the CC.
                if (cc->_pinValue >= 100) {
                    cc->_runner->kill();
                }
                else {
                    // pinvalue is <100, so there is nobody actively holding the CC.  We can safely
                    // delete it as nobody is holding the CC.
                    shouldDelete = true;
                }
            }
            // Begin cursor-only DEPRECATED
            else if (cc->c()->shouldDestroyOnNSDeletion()) {
                verify(NULL == cc->_runner.get());

                if (isDB) {
                    // already checked that db matched above
                    dassert(str::startsWith(cc->_ns.c_str(), ns));
                    shouldDelete = true;
                }
                else {
                    if (str::equals(cc->_ns.c_str(), ns)) {
                        shouldDelete = true;
                    }
                }
            }
            // End cursor-only DEPRECATED

            if (shouldDelete) {
                ClientCursor* toDelete = it->second;
                CursorId id = toDelete->cursorid();
                delete toDelete;
                // We're not following the usual paradigm of saving it, ++it, and deleting the saved
                // 'it' because deleting 'it' might invalidate the next thing in clientCursorsById.
                // TODO: Why?
                it = clientCursorsById.upper_bound(id);
            }
            else {
                ++it;
            }
        }
    }

    /* must call this on a delete so we clean up the cursors. */
    void ClientCursor::aboutToDelete(const StringData& ns,
                                     const NamespaceDetails* nsd,
                                     const DiskLoc& dl) {
        // Begin cursor-only
        NoPageFaultsAllowed npfa;
        // End cursor-only

        recursive_scoped_lock lock(ccmutex);

        Database *db = cc().database();
        verify(db);

        aboutToDeleteForSharding( ns, db, nsd, dl );

        // Check our non-cached active runner list.
        for (set<Runner*>::iterator it = nonCachedRunners.begin(); it != nonCachedRunners.end();
             ++it) {

            Runner* runner = *it;
            if (0 == ns.compare(runner->ns())) {
                runner->invalidate(dl);
            }
        }

        // TODO: This requires optimization.  We walk through *all* CCs and send the delete to every
        // CC open on the db we're deleting from.  We could:
        // 1. Map from ns to open runners,
        // 2. Map from ns -> (a map of DiskLoc -> runners who care about that DL)
        //
        // We could also queue invalidations somehow and have them processed later in the runner's
        // read locks.
        for (CCById::const_iterator it = clientCursorsById.begin(); it != clientCursorsById.end();
             ++it) {

            ClientCursor* cc = it->second;
            // We're only interested in cursors over one db.
            if (cc->_db != db) { continue; }
            if (NULL == cc->_runner.get()) { continue; }
            cc->_runner->invalidate(dl);
        }

        // Begin cursor-only
        CCByLoc& bl = db->ccByLoc();
        CCByLoc::iterator j = bl.lower_bound(ByLocKey::min(dl));
        CCByLoc::iterator stop = bl.upper_bound(ByLocKey::max(dl));
        if ( j == stop )
            return;

        vector<ClientCursor*> toAdvance;

        while ( 1 ) {
            toAdvance.push_back(j->second);
            DEV verify( j->first.loc == dl );
            ++j;
            if ( j == stop )
                break;
        }

        if( toAdvance.size() >= 3000 ) {
            log() << "perf warning MPW101: " << toAdvance.size() << " cursors for one diskloc "
                  << dl.toString()
                  << ' ' << toAdvance[1000]->_ns
                  << ' ' << toAdvance[2000]->_ns
                  << ' ' << toAdvance[1000]->_pinValue
                  << ' ' << toAdvance[2000]->_pinValue
                  << ' ' << toAdvance[1000]->_pos
                  << ' ' << toAdvance[2000]->_pos
                  << ' ' << toAdvance[1000]->_idleAgeMillis
                  << ' ' << toAdvance[2000]->_idleAgeMillis
                  << ' ' << toAdvance[1000]->_doingDeletes
                  << ' ' << toAdvance[2000]->_doingDeletes
                  << endl;
            //wassert( toAdvance.size() < 5000 );
        }

        for ( vector<ClientCursor*>::iterator i = toAdvance.begin(); i != toAdvance.end(); ++i ) {
            ClientCursor* cc = *i;
            wassert(cc->_db == db);

            if ( cc->_doingDeletes ) continue;

            Cursor *c = cc->_c.get();
            if ( c->capped() ) {
                /* note we cannot advance here. if this condition occurs, writes to the oplog
                   have "caught" the reader.  skipping ahead, the reader would miss postentially
                   important data.
                   */
                delete cc;
                continue;
            }

            c->recoverFromYield();
            DiskLoc tmp1 = c->refLoc();
            if ( tmp1 != dl ) {
                // This might indicate a failure to call ClientCursor::prepareToYield() but it can
                // also happen during correct operation, see SERVER-2009.
                problem() << "warning: cursor loc " << tmp1 << " does not match byLoc position " << dl << " !" << endl;
            }
            else {
                c->advance();
            }
            while (!c->eof() && c->refLoc() == dl) {
                /* We don't delete at EOF because we want to return "no more results" rather than "no such cursor".
                 * The loop is to handle MultiKey indexes where the deleted record is pointed to by multiple adjacent keys.
                 * In that case we need to advance until we get to the next distinct record or EOF.
                 * SERVER-4154
                 * SERVER-5198
                 * But see SERVER-5725.
                 */
                c->advance();
            }
            cc->updateLocation();
        }
        // End cursor-only
    }

    void ClientCursor::registerRunner(Runner* runner) {
        recursive_scoped_lock lock(ccmutex);
        verify(nonCachedRunners.end() == nonCachedRunners.find(runner));
        nonCachedRunners.insert(runner);
    }

    void ClientCursor::deregisterRunner(Runner* runner) {
        recursive_scoped_lock lock(ccmutex);
        verify(nonCachedRunners.end() != nonCachedRunners.find(runner));
        nonCachedRunners.erase(runner);
    }

    void yieldOrSleepFor1Microsecond() {
#ifdef _WIN32
        SwitchToThread();
#elif defined(__linux__)
        pthread_yield();
#else
        sleepmicros(1);
#endif
    }

    void ClientCursor::staticYield(int micros, const StringData& ns, Record* rec) {
        bool haveReadLock = Lock::isReadLocked();

        killCurrentOp.checkForInterrupt( false );
        {
            auto_ptr<LockMongoFilesShared> lk;
            if ( rec ) {
                // need to lock this else rec->touch won't be safe file could disappear
                lk.reset( new LockMongoFilesShared() );
            }

            dbtempreleasecond unlock;
            if ( unlock.unlocked() ) {
                if ( haveReadLock ) {
                    // This sleep helps reader threads yield to writer threads.
                    // Without this, the underlying reader/writer lock implementations
                    // are not sufficiently writer-greedy.
#ifdef _WIN32
                    SwitchToThread();
#else
                    if ( micros == 0 ) {
                        yieldOrSleepFor1Microsecond();
                    }
                    else {
                        sleepmicros(1);
                    }
#endif
                }
                else {
                    if ( micros == -1 ) {
                        sleepmicros(Client::recommendedYieldMicros());
                    }
                    else if ( micros == 0 ) {
                        yieldOrSleepFor1Microsecond();
                    }
                    else if ( micros > 0 ) {
                        sleepmicros( micros );
                    }
                }

            }
            else if ( Listener::getTimeTracker() == 0 ) {
                // we aren't running a server, so likely a repair, so don't complain
            }
            else {
                CurOp * c = cc().curop();
                while ( c->parent() )
                    c = c->parent();
                warning() << "ClientCursor::yield can't unlock b/c of recursive lock"
                          << " ns: " << ns 
                          << " top: " << c->info()
                          << endl;
            }

            if ( rec )
                rec->touch();

            lk.reset(0); // need to release this before dbtempreleasecond
        }
    }

    //
    // Timing and timeouts
    //

    bool ClientCursor::shouldTimeout(unsigned millis) {
        _idleAgeMillis += millis;
        return _idleAgeMillis > 600000 && _pinValue == 0;
    }

    void ClientCursor::idleTimeReport(unsigned millis) {
        bool foundSomeToTimeout = false;

        // two passes so that we don't need to readlock unless we really do some timeouts
        // we assume here that incrementing _idleAgeMillis outside readlock is ok.
        {
            recursive_scoped_lock lock(ccmutex);
            {
                unsigned sz = clientCursorsById.size();
                static time_t last;
                if( sz >= 100000 ) { 
                    if( time(0) - last > 300 ) {
                        last = time(0);
                        log() << "warning number of open cursors is very large: " << sz << endl;
                    }
                }
            }
            for ( CCById::iterator i = clientCursorsById.begin(); i != clientCursorsById.end();  ) {
                CCById::iterator j = i;
                i++;
                if( j->second->shouldTimeout( millis ) ) {
                    foundSomeToTimeout = true;
                }
            }
        }

        if( foundSomeToTimeout ) {
            Lock::GlobalRead lk;

            recursive_scoped_lock cclock(ccmutex);
            CCById::const_iterator it = clientCursorsById.begin();
            while (it != clientCursorsById.end()) {
                ClientCursor* cc = it->second;
                if( cc->shouldTimeout(0) ) {
                    numberTimedOut++;
                    LOG(1) << "killing old cursor " << cc->_cursorid << ' ' << cc->_ns
                           << " idle:" << cc->idleTime() << "ms\n";
                    ClientCursor* toDelete = it->second;
                    CursorId id = toDelete->cursorid();
                    // This is what winds up removing it from the map.
                    delete toDelete;
                    it = clientCursorsById.upper_bound(id);
                }
                else {
                    ++it;
                }
            }
        }
    }

    // DEPRECATED only used by Cursor.
    void ClientCursor::storeOpForSlave( DiskLoc last ) {
        verify(NULL == _runner.get());
        if ( ! ( _queryOptions & QueryOption_OplogReplay ))
            return;

        if ( last.isNull() )
            return;

        BSONElement e = last.obj()["ts"];
        if ( e.type() == Date || e.type() == Timestamp )
            _slaveReadTill = e._opTime();
    }

    void ClientCursor::updateSlaveLocation( CurOp& curop ) {
        if ( _slaveReadTill.isNull() )
            return;
        mongo::updateSlaveLocation( curop , _ns.c_str() , _slaveReadTill );
    }

    void ClientCursor::appendStats( BSONObjBuilder& result ) {
        recursive_scoped_lock lock(ccmutex);
        result.appendNumber("totalOpen", clientCursorsById.size() );
        result.appendNumber("clientCursors_size", (int) numCursors());
        result.appendNumber("timedOut" , numberTimedOut);
        unsigned pinned = 0;
        unsigned notimeout = 0;
        for ( CCById::iterator i = clientCursorsById.begin(); i != clientCursorsById.end(); i++ ) {
            unsigned p = i->second->_pinValue;
            if( p >= 100 )
                pinned++;
            else if( p > 0 )
                notimeout++;
        }
        if( pinned ) 
            result.append("pinned", pinned);
        if( notimeout )
            result.append("totalNoTimeout", notimeout);
    }

    //
    // ClientCursor creation/deletion/access.
    //

    // Some statics used by allocCursorId_inlock().
    namespace {
        // so we don't have to do find() which is a little slow very often.
        long long cursorGenTSLast = 0;
        PseudoRandom* cursorGenRandom = NULL;
    }

    long long ClientCursor::allocCursorId_inlock() {
        // It is important that cursor IDs not be reused within a short period of time.
        if (!cursorGenRandom) {
            scoped_ptr<SecureRandom> sr( SecureRandom::create() );
            cursorGenRandom = new PseudoRandom( sr->nextInt64() );
        }

        const long long ts = Listener::getElapsedTimeMillis();
        long long x;
        while ( 1 ) {
            x = ts << 32;
            x |= cursorGenRandom->nextInt32();

            if ( x == 0 ) { continue; }

            if ( x < 0 ) { x *= -1; }

            if ( ts != cursorGenTSLast || ClientCursor::find_inlock(x, false) == 0 )
                break;
        }

        cursorGenTSLast = ts;
        return x;
    }

    // static
    ClientCursor* ClientCursor::find_inlock(CursorId id, bool warn) {
        CCById::iterator it = clientCursorsById.find(id);
        if ( it == clientCursorsById.end() ) {
            if ( warn ) {
                OCCASIONALLY out() << "ClientCursor::find(): cursor not found in map '" << id
                    << "' (ok after a drop)" << endl;
            }
            return 0;
        }
        return it->second;
    }

    void ClientCursor::find( const string& ns , set<CursorId>& all ) {
        recursive_scoped_lock lock(ccmutex);

        for ( CCById::iterator i=clientCursorsById.begin(); i!=clientCursorsById.end(); ++i ) {
            if ( i->second->_ns == ns )
                all.insert( i->first );
        }
    }

    // static
    ClientCursor* ClientCursor::find(CursorId id, bool warn) {
        recursive_scoped_lock lock(ccmutex);
        ClientCursor *c = find_inlock(id, warn);
        // if this asserts, your code was not thread safe - you either need to set no timeout
        // for the cursor or keep a ClientCursor::Pointer in scope for it.
        massert( 12521, "internal error: use of an unlocked ClientCursor", c == 0 || c->_pinValue );
        return c;
    }

    void ClientCursor::_erase_inlock(ClientCursor* cursor) {
        // Must not have an active ClientCursor::Pin.
        massert( 16089,
                str::stream() << "Cannot kill active cursor " << cursor->cursorid(),
                cursor->_pinValue < 100 );

        delete cursor;
    }

    bool ClientCursor::erase(CursorId id) {
        recursive_scoped_lock lock(ccmutex);
        ClientCursor* cursor = find_inlock(id);
        if (!cursor) { return false; }
        _erase_inlock(cursor);
        return true;
    }

    int ClientCursor::erase(int n, long long *ids) {
        int found = 0;
        for ( int i = 0; i < n; i++ ) {
            if ( erase(ids[i]))
                found++;

            if ( inShutdown() )
                break;
        }
        return found;
    }

    bool ClientCursor::eraseIfAuthorized(CursorId id) {
        std::string ns;
        {
            recursive_scoped_lock lock(ccmutex);
            ClientCursor* cursor = find_inlock(id);
            if (!cursor) {
                audit::logKillCursorsAuthzCheck(
                        &cc(),
                        NamespaceString(""),
                        id,
                        ErrorCodes::CursorNotFound);
                return false;
            }
            ns = cursor->ns();
        }

        // Can't be in a lock when checking authorization
        const bool isAuthorized = cc().getAuthorizationSession()->checkAuthorization(
                ns, ActionType::killCursors);
        audit::logKillCursorsAuthzCheck(
                &cc(),
                NamespaceString(ns),
                id,
                isAuthorized ? ErrorCodes::OK : ErrorCodes::Unauthorized);
        if (!isAuthorized) {
            return false;
        }

        // It is safe to lookup the cursor again after temporarily releasing the mutex because
        // of 2 invariants: that the cursor ID won't be re-used in a short period of time, and that
        // the namespace associated with a cursor cannot change.
        recursive_scoped_lock lock(ccmutex);
        ClientCursor* cursor = find_inlock(id);
        if (!cursor) {
            // Cursor was deleted in another thread since we found it earlier in this function.
            return false;
        }
        if (cursor->ns() != ns) {
            warning() << "Cursor namespace changed. Previous ns: " << ns << ", current ns: "
                    << cursor->ns() << endl;
            return false;
        }

        _erase_inlock(cursor);
        return true;
    }

    int ClientCursor::eraseIfAuthorized(int n, long long *ids) {
        int found = 0;
        for ( int i = 0; i < n; i++ ) {
            if ( eraseIfAuthorized(ids[i]))
                found++;

            if ( inShutdown() )
                break;
        }
        return found;
    }

    //
    // Yielding that is DEPRECATED.  Will be removed when we use runners and they yield internally.
    //

    Record* ClientCursor::_recordForYield( ClientCursor::RecordNeeds need ) {
        if ( ! ok() )
            return 0;

        if ( need == DontNeed ) {
            return 0;
        }
        else if ( need == MaybeCovered ) {
            // TODO
            return 0;
        }
        else if ( need == WillNeed ) {
            // no-op
        }
        else {
            warning() << "don't understand RecordNeeds: " << (int)need << endl;
            return 0;
        }

        DiskLoc l = currLoc();
        if ( l.isNull() )
            return 0;
        
        Record * rec = l.rec();
        if ( rec->likelyInPhysicalMemory() ) 
            return 0;
        
        return rec;
    }

    void ClientCursor::updateLocation() {
        verify( _cursorid );
        _idleAgeMillis = 0;
        // Cursor-specific
        _c->prepareToYield();
        DiskLoc cl = _c->refLoc();
        if ( lastLoc() == cl ) {
            //log() << "info: lastloc==curloc " << ns << endl;
        }
        else {
            recursive_scoped_lock lock(ccmutex);
            setLastLoc_inlock(cl);
        }
    }

    void ClientCursor::setLastLoc_inlock(DiskLoc L) {
        verify(NULL == _runner.get());
        verify( _pos != -2 ); // defensive - see ~ClientCursor

        if (L == _lastLoc) { return; }
        CCByLoc& bl = _db->ccByLoc();

        if (!_lastLoc.isNull()) {
            bl.erase(ByLocKey(_lastLoc, _cursorid));
        }

        if (!L.isNull()) {
            bl[ByLocKey(L,_cursorid)] = this;
        }

        _lastLoc = L;
    }

    bool ClientCursor::yield( int micros , Record * recordToLoad ) {
        // some cursors (geo@oct2011) don't support yielding
        if (!_c->supportYields())  { return true; }

        YieldData data;
        prepareToYield( data );
        staticYield( micros , _ns , recordToLoad );
        return ClientCursor::recoverFromYield( data );
    }

    bool ClientCursor::yieldSometimes(RecordNeeds need, bool* yielded) {
        if (yielded) { *yielded = false; }

        if ( ! _yieldSometimesTracker.intervalHasElapsed() ) {
            Record* rec = _recordForYield( need );
            if ( rec ) {
                // yield for page fault
                if ( yielded ) {
                    *yielded = true;   
                }
                bool res = yield( suggestYieldMicros() , rec );
                if ( res )
                    _yieldSometimesTracker.resetLastTime();
                return res;
            }
            return true;
        }

        int micros = suggestYieldMicros();
        if ( micros > 0 ) {
            if ( yielded ) {
                *yielded = true;   
            }
            bool res = yield( micros , _recordForYield( need ) );
            if ( res ) 
                _yieldSometimesTracker.resetLastTime();
            return res;
        }
        return true;
    }

    bool ClientCursor::prepareToYield( YieldData &data ) {
        if (!_c->supportYields()) { return false; }

        // need to store in case 'this' gets deleted
        data._id = _cursorid;
        data._doingDeletes = _doingDeletes;
        _doingDeletes = false;

        updateLocation();

        {
            /* a quick test that our temprelease is safe.
             todo: make a YieldingCursor class
             and then make the following code part of a unit test.
             */
            const int test = 0;
            static bool inEmpty = false;
            if( test && !inEmpty ) {
                inEmpty = true;
                log() << "TEST: manipulate collection during cc:yield" << endl;
                if( test == 1 )
                    Helpers::emptyCollection(_ns.c_str());
                else if( test == 2 ) {
                    BSONObjBuilder b; string m;
                    dropCollection(_ns.c_str(), m, b);
                }
                else {
                    dropDatabase(_ns.c_str());
                }
            }
        }
        return true;
    }

    bool ClientCursor::recoverFromYield( const YieldData &data ) {
        ClientCursor *cc = ClientCursor::find( data._id , false );
        if ( cc == 0 ) {
            // id was deleted
            return false;
        }

        cc->_doingDeletes = data._doingDeletes;
        cc->_c->recoverFromYield();
        return true;
    }

    int ClientCursor::suggestYieldMicros() {
        int writers = 0;
        int readers = 0;

        int micros = Client::recommendedYieldMicros( &writers , &readers );

        if ( micros > 0 && writers == 0 && Lock::isR() ) {
            // we have a read lock, and only reads are coming on, so why bother unlocking
            return 0;
        }

        wassert( micros < 10000000 );
        dassert( micros <  1000001 );
        return micros;
    }

    //
    // Pin methods
    // TODO: Simplify when we kill Cursor.  In particular, once we've pinned a CC, it won't be
    // deleted from underneath us, so we can save the pointer and ignore the ID.
    //

    ClientCursorPin::ClientCursorPin(long long cursorid) : _cursorid( INVALID_CURSOR_ID ) {
        recursive_scoped_lock lock( ClientCursor::ccmutex );
        ClientCursor *cursor = ClientCursor::find_inlock( cursorid, true );
        if (NULL != cursor) {
            uassert( 12051, "clientcursor already in use? driver problem?",
                    cursor->_pinValue < 100 );
            cursor->_pinValue += 100;
            _cursorid = cursorid;
        }
    }

    ClientCursorPin::~ClientCursorPin() { DESTRUCTOR_GUARD( release(); ) }

    void ClientCursorPin::release() {
        if ( _cursorid == INVALID_CURSOR_ID ) {
            return;
        }
        ClientCursor *cursor = c();
        _cursorid = INVALID_CURSOR_ID;
        if ( cursor ) {
            verify( cursor->_pinValue >= 100 );
            cursor->_pinValue -= 100;
        }
    }

    void ClientCursorPin::free() {
        if (_cursorid == INVALID_CURSOR_ID) {
            return;
        }
        ClientCursor *cursor = c();
        _cursorid = INVALID_CURSOR_ID;
        delete cursor;
    }

    ClientCursor* ClientCursorPin::c() const {
        return ClientCursor::find( _cursorid );
    }

    //
    // Holder methods DEPRECATED
    //
    ClientCursorHolder::ClientCursorHolder(ClientCursor *c) : _c(0), _id(INVALID_CURSOR_ID) {
        reset(c);
    }

    ClientCursorHolder::~ClientCursorHolder() {
        DESTRUCTOR_GUARD(reset(););
    }

    void ClientCursorHolder::reset(ClientCursor *c) {
        if ( c == _c )
            return;
        if ( _c ) {
            // be careful in case cursor was deleted by someone else
            ClientCursor::erase( _id );
        }
        if ( c ) {
            _c = c;
            _id = c->_cursorid;
        }
        else {
            _c = 0;
            _id = INVALID_CURSOR_ID;
        }
    }
    ClientCursor* ClientCursorHolder::get() { return _c; }
    ClientCursor * ClientCursorHolder::operator-> () { return _c; }
    const ClientCursor * ClientCursorHolder::operator-> () const { return _c; }
    void ClientCursorHolder::release() {
        _c = 0;
        _id = INVALID_CURSOR_ID;
    }

    //
    // ClientCursorMonitor
    //

    // Used by sayMemoryStatus below.
    struct Mem { 
        Mem() { res = virt = mapped = 0; }
        long long res;
        long long virt;
        long long mapped;
        bool grew(const Mem& r) { 
            return (r.res && (((double)res)/r.res)>1.1 ) ||
              (r.virt && (((double)virt)/r.virt)>1.1 ) ||
              (r.mapped && (((double)mapped)/r.mapped)>1.1 );
        }
    };

    /**
     * called once a minute from killcursors thread
     */
    void sayMemoryStatus() { 
        static time_t last;
        static Mem mlast;
        try {
            ProcessInfo p;
            if ( !cmdLine.quiet && p.supported() ) {
                Mem m;
                m.res = p.getResidentSize();
                m.virt = p.getVirtualMemorySize();
                m.mapped = MemoryMappedFile::totalMappedLength() / (1024 * 1024);
                time_t now = time(0);
                if( now - last >= 300 || m.grew(mlast) ) { 
                    log() << "mem (MB) res:" << m.res << " virt:" << m.virt;
                    long long totalMapped = m.mapped;
                    if (cmdLine.dur) {
                        totalMapped *= 2;
                        log() << " mapped (incl journal view):" << totalMapped;
                    }
                    else {
                        log() << " mapped:" << totalMapped;
                    }
                    log() << " connections:" << Listener::globalTicketHolder.used();
                    if (theReplSet) {
                        log() << " replication threads:" << 
                            ReplSetImpl::replWriterThreadCount + 
                            ReplSetImpl::replPrefetcherThreadCount;
                    }
                    last = now;
                    mlast = m;
                }
            }
        }
        catch(const std::exception&) {
            log() << "ProcessInfo exception" << endl;
        }
    }

    void ClientCursorMonitor::run() {
        Client::initThread("clientcursormon");
        Client& client = cc();
        Timer t;
        const int Secs = 4;
        unsigned n = 0;
        while ( ! inShutdown() ) {
            ClientCursor::idleTimeReport( t.millisReset() );
            sleepsecs(Secs);
            if( ++n % (60/4) == 0 /*once a minute*/ ) { 
                sayMemoryStatus();
            }
        }
        client.shutdown();
    }

    ClientCursorMonitor clientCursorMonitor;

    //
    // cursorInfo command.
    //

    // QUESTION: Restrict to the namespace from which this command was issued?
    // Alternatively, make this command admin-only?
    class CmdCursorInfo : public Command {
    public:
        CmdCursorInfo() : Command( "cursorInfo", true ) {}
        virtual bool slaveOk() const { return true; }
        virtual void help( stringstream& help ) const {
            help << " example: { cursorInfo : 1 }";
        }
        virtual LockType locktype() const { return NONE; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::cursorInfo);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }
        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result,
                 bool fromRepl ) {
            ClientCursor::appendStats( result );
            return true;
        }
    } cmdCursorInfo;

    //
    // cursors stats.
    //

    class CursorServerStats : public ServerStatusSection {
    public:
        CursorServerStats() : ServerStatusSection( "cursors" ){}
        virtual bool includeByDefault() const { return true; }

        BSONObj generateSection(const BSONElement& configElement) const {
            BSONObjBuilder b;
            ClientCursor::appendStats( b );
            return b.obj();
        }
    } cursorServerStats;

    //
    // YieldLock
    //

    ClientCursorYieldLock::ClientCursorYieldLock( ptr<ClientCursor> cc )
        : _canYield(cc->_c->supportYields()) {
     
        if ( _canYield ) {
            cc->prepareToYield( _data );
            _unlock.reset(new dbtempreleasecond());
        }

    }
    
    ClientCursorYieldLock::~ClientCursorYieldLock() {
        if ( _unlock ) {
            warning() << "ClientCursorYieldLock not closed properly" << endl;
            relock();
        }
    }
    
    bool ClientCursorYieldLock::stillOk() {
        if ( ! _canYield )
            return true;
        relock();
        return ClientCursor::recoverFromYield( _data );
    }
    
    void ClientCursorYieldLock::relock() {
        _unlock.reset();
    }


} // namespace mongo
