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

#include "mongo/db/clientcursor.h"

#include <string>
#include <time.h>
#include <vector>

#include "mongo/base/counter.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/db.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/pagefault.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/repl/write_concern.h"

namespace mongo {

    static Counter64 cursorStatsOpen; // gauge
    static Counter64 cursorStatsOpenPinned; // gauge
    static Counter64 cursorStatsOpenNoTimeout; // gauge
    static Counter64 cursorStatsTimedOut;

    static ServerStatusMetricField<Counter64> dCursorStatsOpen( "cursor.open.total",
                                                                        &cursorStatsOpen );
    static ServerStatusMetricField<Counter64> dCursorStatsOpenPinned( "cursor.open.pinned",
                                                                      &cursorStatsOpenPinned );
    static ServerStatusMetricField<Counter64> dCursorStatsOpenNoTimeout( "cursor.open.noTimeout",
                                                                         &cursorStatsOpenNoTimeout );
    static ServerStatusMetricField<Counter64> dCursorStatusTimedout( "cursor.timedOut",
                                                                     &cursorStatsTimedOut );

    long long ClientCursor::totalOpen() {
        return cursorStatsOpen.get();
    }

    ClientCursor::ClientCursor(const Collection* collection, Runner* runner,
                               int qopts, const BSONObj query)
        : _collection( collection ),
          _countedYet( false ) {
        _runner.reset(runner);
        _ns = runner->ns();
        _query = query;
        _queryOptions = qopts;
        if ( runner->collection() ) {
            invariant( collection == runner->collection() );
        }
        init();
    }

    ClientCursor::ClientCursor(const Collection* collection)
        : _ns(collection->ns().ns()),
          _collection(collection),
          _countedYet( false ),
          _queryOptions(QueryOption_NoCursorTimeout) {
        init();
    }

    void ClientCursor::init() {
        invariant( _collection );

        isAggCursor = false;

        _idleAgeMillis = 0;
        _leftoverMaxTimeMicros = 0;
        _pinValue = 0;
        _pos = 0;

        Lock::assertAtLeastReadLocked(_ns);

        if (_queryOptions & QueryOption_NoCursorTimeout) {
            // cursors normally timeout after an inactivity period to prevent excess memory use
            // setting this prevents timeout of the cursor in question.
            ++_pinValue;
            cursorStatsOpenNoTimeout.increment();
        }

        _cursorid = _collection->cursorCache()->registerCursor( this );

        cursorStatsOpen.increment();
        _countedYet = true;
    }

    ClientCursor::~ClientCursor() {
        if( _pos == -2 ) {
            // defensive: destructor called twice
            wassert(false);
            return;
        }

        if ( _countedYet ) {
            _countedYet = false;
            cursorStatsOpen.decrement();
            if ( _pinValue == 1 )
                cursorStatsOpenNoTimeout.decrement();
        }

        if ( _collection ) {
            // this could be null if kill() was killed
            _collection->cursorCache()->deregisterCursor( this );
        }

        // defensive:
        _collection = NULL;
        _cursorid = INVALID_CURSOR_ID;
        _pos = -2;
        _pinValue = 0;
    }

    void ClientCursor::kill() {
        if ( _runner.get() )
            _runner->kill();

        _collection = NULL;
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

    void ClientCursor::staticYield(int micros, const StringData& ns, const Record* rec) {
        bool haveReadLock = Lock::isReadLocked();

        killCurrentOp.checkForInterrupt();
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
                warning() << "ClientCursor::staticYield can't unlock b/c of recursive lock"
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

    void ClientCursor::setIdleTime( unsigned millis ) {
        _idleAgeMillis = millis;
    }

    void ClientCursor::updateSlaveLocation( CurOp& curop ) {
        if ( _slaveReadTill.isNull() )
            return;
        mongo::updateSlaveLocation( curop , _ns.c_str() , _slaveReadTill );
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

    ClientCursorPin::ClientCursorPin( const Collection* collection, long long cursorid )
        : _cursor( NULL ) {
        cursorStatsOpenPinned.increment();
        _cursor = collection->cursorCache()->find( cursorid, true );
    }

    ClientCursorPin::~ClientCursorPin() {
        cursorStatsOpenPinned.decrement();
        DESTRUCTOR_GUARD( release(); );
    }

    void ClientCursorPin::release() {
        if ( !_cursor )
            return;

        invariant( _cursor->pinValue() >= 100 );

        if ( _cursor->collection() == NULL ) {
            // the ClientCursor was killed while we had it
            // therefore its our responsibility to kill it
            delete _cursor;
            _cursor = NULL; // defensive
        }
        else {
            _cursor->collection()->cursorCache()->unpin( _cursor );
        }
    }

    void ClientCursorPin::deleteUnderlying() {
        delete _cursor;
        _cursor = NULL;
    }

    ClientCursor* ClientCursorPin::c() const {
        return _cursor;
    }

    //
    // ClientCursorMonitor
    //

    void ClientCursorMonitor::run() {
        Client::initThread("clientcursormon");
        Client& client = cc();
        Timer t;
        const int Secs = 4;
        while ( ! inShutdown() ) {
            cursorStatsTimedOut.increment( CollectionCursorCache::timeoutCursorsGlobal( t.millisReset() ) );
            sleepsecs(Secs);
        }
        client.shutdown();
    }

    namespace {
        ClientCursorMonitor clientCursorMonitor;
        
        void _appendCursorStats( BSONObjBuilder& b ) {
            b.append( "note" , "deprecated, use server status metrics" );
            b.appendNumber("clientCursors_size", cursorStatsOpen.get() );
            b.appendNumber("totalOpen", cursorStatsOpen.get() );
            b.appendNumber("pinned", cursorStatsOpenPinned.get() );
            b.appendNumber("totalNoTimeout", cursorStatsOpenNoTimeout.get() );
            b.appendNumber("timedOut" , cursorStatsTimedOut.get());
        }
    }

    // QUESTION: Restrict to the namespace from which this command was issued?
    // Alternatively, make this command admin-only?
    // TODO: remove this for 2.8
    class CmdCursorInfo : public Command {
    public:
        CmdCursorInfo() : Command( "cursorInfo", true ) {}
        virtual bool slaveOk() const { return true; }
        virtual void help( stringstream& help ) const {
            help << " example: { cursorInfo : 1 }, deprecated";
        }
        virtual LockType locktype() const { return NONE; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::cursorInfo);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }
        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result,
                 bool fromRepl ) {
            _appendCursorStats( result );
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
            _appendCursorStats( b );
            return b.obj();
        }
    } cursorServerStats;

} // namespace mongo
