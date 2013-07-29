/**
*    Copyright (C) 2008 10gen Inc.
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

/* clientcursor.cpp

   ClientCursor is a wrapper that represents a cursorid from our database
   application's perspective.

   Cursor -- and its derived classes -- are our internal cursors.
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

    CCById ClientCursor::clientCursorsById;
    boost::recursive_mutex& ClientCursor::ccmutex( *(new boost::recursive_mutex()) );
    long long ClientCursor::numberTimedOut = 0;

    void aboutToDeleteForSharding( const StringData& ns,
                                   const Database* db,
                                   const NamespaceDetails* nsd,
                                   const DiskLoc& dl ); // from s/d_logic.h

    /*static*/ void ClientCursor::assertNoCursors() {
        recursive_scoped_lock lock(ccmutex);
        if( clientCursorsById.size() ) {
            log() << "ERROR clientcursors exist but should not at this point" << endl;
            ClientCursor *cc = clientCursorsById.begin()->second;
            log() << "first one: " << cc->_cursorid << ' ' << cc->_ns << endl;
            clientCursorsById.clear();
            verify(false);
        }
    }


    void ClientCursor::setLastLoc_inlock(DiskLoc L) {
        verify( _pos != -2 ); // defensive - see ~ClientCursor

        if ( L == _lastLoc )
            return;

        CCByLoc& bl = _db->ccByLoc();

        if ( !_lastLoc.isNull() ) {
            bl.erase( ByLocKey( _lastLoc, _cursorid ) );
        }

        if ( !L.isNull() )
            bl[ByLocKey(L,_cursorid)] = this;
        _lastLoc = L;
    }

    /* ------------------------------------------- */

    /* must call this when a btree node is updated */
    //void removedKey(const DiskLoc& btreeLoc, int keyPos) {
    //}

    // ns is either a full namespace or "dbname." when invalidating for a whole db
    void ClientCursor::invalidate(const char *ns) {
        Lock::assertWriteLocked(ns);
        int len = strlen(ns);
        const char* dot = strchr(ns, '.');
        verify( len > 0 && dot);

        bool isDB = (dot == &ns[len-1]); // first (and only) dot is the last char

        {
            //cout << "\nTEMP invalidate " << ns << endl;
            Database *db = cc().database();
            verify(db);
            verify( str::startsWith(ns, db->name()) );

            for( LockedIterator i; i.ok(); ) {
                ClientCursor *cc = i.current();

                bool shouldDelete = false;
                if (cc->c()->shouldDestroyOnNSDeletion() && cc->_db == db) {
                    if (isDB) {
                        // already checked that db matched above
                        dassert( str::startsWith(cc->_ns.c_str(), ns) );
                        shouldDelete = true;
                    }
                    else {
                        if ( str::equals(cc->_ns.c_str(), ns) )
                            shouldDelete = true;
                    }
                }

                if ( shouldDelete ) {
                    i.deleteAndAdvance();
                }
                else {
                    i.advance();
                }
            }

            /*
            note : we can't iterate byloc because clientcursors may exist with a loc of null in which case
                   they are not in the map.  perhaps they should not exist though in the future?  something to
                   change???

            CCByLoc& bl = db->ccByLoc;
            for ( CCByLoc::iterator i = bl.begin(); i != bl.end(); ++i ) {
                ClientCursor *cc = i->second;
                if ( strncmp(ns, cc->ns.c_str(), len) == 0 ) {
                    verify( cc->_db == db );
                    toDelete.push_back(i->second);
                }
            }*/

            /*cout << "TEMP after invalidate " << endl;
            for( auto i = clientCursorsById.begin(); i != clientCursorsById.end(); ++i ) {
                cout << "  " << i->second->ns << endl;
            }
            cout << "TEMP after invalidate done" << endl;*/
        }
    }

    /* note called outside of locks (other than ccmutex) so care must be exercised */
    bool ClientCursor::shouldTimeout( unsigned millis ) {
        _idleAgeMillis += millis;
        return _idleAgeMillis > 600000 && _pinValue == 0;
    }

    /* called every 4 seconds.  millis is amount of idle time passed since the last call -- could be zero */
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
            for( LockedIterator i; i.ok(); ) {
                ClientCursor *cc = i.current();
                if( cc->shouldTimeout(0) ) {
                    numberTimedOut++;
                    LOG(1) << "killing old cursor " << cc->_cursorid << ' ' << cc->_ns
                           << " idle:" << cc->idleTime() << "ms\n";
                    i.deleteAndAdvance();
                }
                else {
                    i.advance();
                }
            }
        }
    }

    /* must call this on a delete so we clean up the cursors. */
    void ClientCursor::aboutToDelete( const StringData& ns,
                                      const NamespaceDetails* nsd,
                                      const DiskLoc& dl )
    {
        NoPageFaultsAllowed npfa;

        recursive_scoped_lock lock(ccmutex);

        Database *db = cc().database();
        verify(db);

        aboutToDeleteForSharding( ns, db, nsd, dl );

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
    }

    void ClientCursor::LockedIterator::deleteAndAdvance() {
        ClientCursor *cc = current();
        CursorId id = cc->cursorid();
        delete cc;
        _i = clientCursorsById.upper_bound( id );
    }
    
    ClientCursor::ClientCursor(int queryOptions, const shared_ptr<Cursor>& c, const string& ns, BSONObj query ) :
        _ns(ns), _db( cc().database() ),
        _c(c), _pos(0),
        _query(query),  _queryOptions(queryOptions),
        _idleAgeMillis(0), _leftoverMaxTimeMicros(0), _pinValue(0),
        _doingDeletes(false), _yieldSometimesTracker(128,10) {

        Lock::assertAtLeastReadLocked(ns);

        verify( _db );
        verify( _db->ownsNS( _ns ) );
        if( queryOptions & QueryOption_NoCursorTimeout )
            noTimeout();
        recursive_scoped_lock lock(ccmutex);
        _cursorid = allocCursorId_inlock();
        clientCursorsById.insert( make_pair(_cursorid, this) );

        if ( ! _c->modifiedKeys() ) {
            // store index information so we can decide if we can
            // get something out of the index key rather than full object

            int x = 0;
            BSONObjIterator i( _c->indexKeyPattern() );
            while ( i.more() ) {
                BSONElement e = i.next();
                if ( e.isNumber() ) {
                    // only want basic index fields, not "2d" etc
                    _indexedFields[e.fieldName()] = x;
                }
                x++;
            }
        }

    }


    ClientCursor::~ClientCursor() {
        if( _pos == -2 ) {
            // defensive: destructor called twice
            wassert(false);
            return;
        }

        {
            recursive_scoped_lock lock(ccmutex);
            setLastLoc_inlock( DiskLoc() ); // removes us from bylocation multimap
            clientCursorsById.erase(_cursorid);

            // defensive:
            _cursorid = INVALID_CURSOR_ID;
            _pos = -2;
            _pinValue = 0;
        }
    }

    bool ClientCursor::getFieldsDotted( const string& name, BSONElementSet &ret, BSONObj& holder ) {

        map<string,int>::const_iterator i = _indexedFields.find( name );
        if ( i == _indexedFields.end() ) {
            current().getFieldsDotted( name , ret );
            return false;
        }

        int x = i->second;

        holder = currKey();
        BSONObjIterator it( holder );
        while ( x && it.more() ) {
            it.next();
            x--;
        }
        verify( x == 0 );
        ret.insert( it.next() );
        return true;
    }

    BSONElement ClientCursor::getFieldDotted( const string& name , BSONObj& holder , bool * fromKey ) {

        map<string,int>::const_iterator i = _indexedFields.find( name );
        if ( i == _indexedFields.end() ) {
            if ( fromKey )
                *fromKey = false;
            holder = current();
            return holder.getFieldDotted( name );
        }
        
        int x = i->second;

        holder = currKey();
        BSONObjIterator it( holder );
        while ( x && it.more() ) {
            it.next();
            x--;
        }
        verify( x == 0 );

        if ( fromKey )
            *fromKey = true;
        return it.next();
    }

    BSONObj ClientCursor::extractFields(const BSONObj &pattern , bool fillWithNull ) {
        BSONObjBuilder b( pattern.objsize() * 2 );

        BSONObj holder;
     
        BSONObjIterator i( pattern ); 
        while ( i.more() ) {
            BSONElement key = i.next();
            BSONElement value = getFieldDotted( key.fieldName() , holder );

            if ( value.type() ) {
                b.appendAs( value , key.fieldName() );
                continue;
            }

            if ( fillWithNull ) 
                b.appendNull( key.fieldName() );
            
        }

        return b.obj();
    }
    
    BSONObj ClientCursor::extractKey( const KeyPattern& usingKeyPattern ) const {
        KeyPattern currentIndex( _c->indexKeyPattern() );
        if ( usingKeyPattern.isCoveredBy( currentIndex ) && ! currentIndex.isSpecial() ){
            BSONObj currKey = _c->currKey();
            BSONObj prettyKey = currKey.replaceFieldNames( currentIndex.toBSON() );
            return usingKeyPattern.extractSingleKey( prettyKey );
        }
        return usingKeyPattern.extractSingleKey( _c->current() );
    }

    void ClientCursor::fillQueryResultFromObj( BufBuilder &b, const MatchDetails* details ) const {
        const Projection::KeyOnly *keyFieldsOnly = c()->keyFieldsOnly();
        if ( keyFieldsOnly ) {
            mongo::fillQueryResultFromObj( b, 0, keyFieldsOnly->hydrate( c()->currKey() ), details );
        }
        else {
            DiskLoc loc = c()->currLoc();
            mongo::fillQueryResultFromObj( b, fields.get(), c()->current(), details,
                                          ( ( pq && pq->showDiskLoc() ) ? &loc : 0 ) );
        }
    }

    /* call when cursor's location changes so that we can update the
       cursorsbylocation map.  if you are locked and internally iterating, only
       need to call when you are ready to "unlock".
    */
    void ClientCursor::updateLocation() {
        verify( _cursorid );
        _idleAgeMillis = 0;
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

    bool ClientCursor::yieldSometimes( RecordNeeds need, bool *yielded ) {
        if ( yielded ) {
            *yielded = false;   
        }
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

    void ClientCursor::staticYield( int micros , const StringData& ns , Record * rec ) {
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
                        pthread_yield();
                    }
                    else {
                        sleepmicros(1);
                    }
#endif
                }
                else {
                    if ( micros == -1 ) {
                        micros = Client::recommendedYieldMicros();
                    }
                    else if ( micros == 0 ) {
#ifdef _WIN32
                        SwitchToThread();
#else
                        pthread_yield();
#endif
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

    bool ClientCursor::prepareToYield( YieldData &data ) {
        if ( ! _c->supportYields() )
            return false;

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

    /** @return true if cursor is still ok */
    bool ClientCursor::yield( int micros , Record * recordToLoad ) {

        if ( ! _c->supportYields() ) // so me cursors (geo@oct2011) don't support yielding
            return true;

        YieldData data;
        prepareToYield( data );
        staticYield( micros , _ns , recordToLoad );
        return ClientCursor::recoverFromYield( data );
    }

    namespace {
        // so we don't have to do find() which is a little slow very often.
        long long cursorGenTSLast = 0;
        PseudoRandom* cursorGenRandom = NULL;
    }

    long long ClientCursor::allocCursorId_inlock() {
        // It is important that cursor IDs not be reused within a short period of time.

        if ( ! cursorGenRandom ) {
            scoped_ptr<SecureRandom> sr( SecureRandom::create() );
            cursorGenRandom = new PseudoRandom( sr->nextInt64() );
        }

        const long long ts = Listener::getElapsedTimeMillis();

        long long x;

        while ( 1 ) {
            x = ts << 32;
            x |= cursorGenRandom->nextInt32();

            if ( x == 0 )
                continue;

            if ( x < 0 )
                x *= -1;

            if ( ts != cursorGenTSLast || ClientCursor::find_inlock(x, false) == 0 )
                break;
        }

        cursorGenTSLast = ts;

        return x;
    }

    void ClientCursor::storeOpForSlave( DiskLoc last ) {
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
        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            ClientCursor::appendStats( result );
            return true;
        }
    } cmdCursorInfo;

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

    /** called once a minute from killcursors thread */
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

    /** thread for timing out old cursors */
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

    void ClientCursor::find( const string& ns , set<CursorId>& all ) {
        recursive_scoped_lock lock(ccmutex);

        for ( CCById::iterator i=clientCursorsById.begin(); i!=clientCursorsById.end(); ++i ) {
            if ( i->second->_ns == ns )
                all.insert( i->first );
        }
    }

    bool ClientCursor::_erase_inlock(ClientCursor* cursor) {
        // Must not have an active ClientCursor::Pin.
        massert( 16089,
                str::stream() << "Cannot kill active cursor " << cursor->cursorid(),
                cursor->_pinValue < 100 );

        delete cursor;
        return true;
    }

    bool ClientCursor::erase(CursorId id) {
        recursive_scoped_lock lock(ccmutex);
        ClientCursor* cursor = find_inlock(id);
        if (!cursor) {
            return false;
        }

        return _erase_inlock(cursor);
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

        return _erase_inlock(cursor);
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

    ClientCursor::YieldLock::YieldLock( ptr<ClientCursor> cc )
        : _canYield(cc->_c->supportYields()) {
     
        if ( _canYield ) {
            cc->prepareToYield( _data );
            _unlock.reset(new dbtempreleasecond());
        }

    }
    
    ClientCursor::YieldLock::~YieldLock() {
        if ( _unlock ) {
            warning() << "ClientCursor::YieldLock not closed properly" << endl;
            relock();
        }
    }
    
    bool ClientCursor::YieldLock::stillOk() {
        if ( ! _canYield )
            return true;
        relock();
        return ClientCursor::recoverFromYield( _data );
    }
    
    void ClientCursor::YieldLock::relock() {
        _unlock.reset();
    }


    ClientCursorMonitor clientCursorMonitor;

} // namespace mongo
