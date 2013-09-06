// cursors.cpp
/*
 *    Copyright (C) 2010 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */


#include "mongo/pch.h"

#include "mongo/s/cursors.h"

#include <string>
#include <vector>

#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/client/connpool.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/concurrency/task.h"
#include "mongo/util/net/listen.h"

namespace mongo {
    const int ShardedClientCursor::INIT_REPLY_BUFFER_SIZE = 32768;

    // --------  ShardedCursor -----------

    ShardedClientCursor::ShardedClientCursor( QueryMessage& q , ClusteredCursor * cursor ) {
        verify( cursor );
        _cursor = cursor;

        _skip = q.ntoskip;
        _ntoreturn = q.ntoreturn;

        _totalSent = 0;
        _done = false;

        _id = 0;

        if ( q.queryOptions & QueryOption_NoCursorTimeout ) {
            _lastAccessMillis = 0;
        }
        else
            _lastAccessMillis = Listener::getElapsedTimeMillis();
    }

    ShardedClientCursor::~ShardedClientCursor() {
        verify( _cursor );
        delete _cursor;
        _cursor = 0;
    }

    long long ShardedClientCursor::getId() {
        if ( _id <= 0 ) {
            _id = cursorCache.genId();
            verify( _id >= 0 );
        }
        return _id;
    }

    int ShardedClientCursor::getTotalSent() const {
        return _totalSent;
    }

    void ShardedClientCursor::accessed() {
        if ( _lastAccessMillis > 0 )
            _lastAccessMillis = Listener::getElapsedTimeMillis();
    }

    long long ShardedClientCursor::idleTime( long long now ) {
        if ( _lastAccessMillis == 0 )
            return 0;
        return now - _lastAccessMillis;
    }

    bool ShardedClientCursor::sendNextBatchAndReply( Request& r ){
        BufBuilder buffer( INIT_REPLY_BUFFER_SIZE );
        int docCount = 0;
        bool hasMore = sendNextBatch( r, _ntoreturn, buffer, docCount );
        replyToQuery( 0, r.p(), r.m(), buffer.buf(), buffer.len(), docCount,
                _totalSent, hasMore ? getId() : 0 );

        return hasMore;
    }

    bool ShardedClientCursor::sendNextBatch( Request& r , int ntoreturn ,
            BufBuilder& buffer, int& docCount ) {
        uassert( 10191 ,  "cursor already done" , ! _done );

        int maxSize = 1024 * 1024;
        if ( _totalSent > 0 )
            maxSize *= 3;

        docCount = 0;

        // Send more if ntoreturn is 0, or any value > 1
        // (one is assumed to be a single doc return, with no cursor)
        bool sendMore = ntoreturn == 0 || ntoreturn > 1;
        ntoreturn = abs( ntoreturn );

        while ( _cursor->more() ) {
            BSONObj o = _cursor->next();

            buffer.appendBuf( (void*)o.objdata() , o.objsize() );
            docCount++;

            if ( buffer.len() > maxSize ) {
                break;
            }

            if ( docCount == ntoreturn ) {
                // soft limit aka batch size
                break;
            }

            if ( ntoreturn == 0 && _totalSent == 0 && docCount >= 100 ) {
                // first batch should be max 100 unless batch size specified
                break;
            }
        }

        bool hasMore = sendMore && _cursor->more();

        LOG(5) << "\t hasMore: " << hasMore
               << " sendMore: " << sendMore
               << " cursorMore: " << _cursor->more()
               << " ntoreturn: " << ntoreturn
               << " num: " << docCount
               << " wouldSendMoreIfHad: " << sendMore
               << " id:" << getId()
               << " totalSent: " << _totalSent << endl;

        _totalSent += docCount;
        _done = ! hasMore;

        return hasMore;
    }

    // ---- CursorCache -----

    long long CursorCache::TIMEOUT = 600000;

    unsigned getCCRandomSeed() {
        scoped_ptr<SecureRandom> sr( SecureRandom::create() );
        return sr->nextInt64();
    }

    CursorCache::CursorCache()
        :_mutex( "CursorCache" ),
         _random( getCCRandomSeed() ),
         _shardedTotal(0) {
    }

    CursorCache::~CursorCache() {
        // TODO: delete old cursors?
        bool print = logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(1));
        if ( _cursors.size() || _refs.size() )
            print = true;
        verify(_refs.size() == _refsNS.size());
        
        if ( print ) 
            log() << " CursorCache at shutdown - "
                  << " sharded: " << _cursors.size()
                  << " passthrough: " << _refs.size()
                  << endl;
    }

    ShardedClientCursorPtr CursorCache::get( long long id ) const {
        LOG(_myLogLevel) << "CursorCache::get id: " << id << endl;
        scoped_lock lk( _mutex );
        MapSharded::const_iterator i = _cursors.find( id );
        if ( i == _cursors.end() ) {
            OCCASIONALLY log() << "Sharded CursorCache missing cursor id: " << id << endl;
            return ShardedClientCursorPtr();
        }
        i->second->accessed();
        return i->second;
    }

    void CursorCache::store( ShardedClientCursorPtr cursor ) {
        LOG(_myLogLevel) << "CursorCache::store cursor " << " id: " << cursor->getId() << endl;
        verify( cursor->getId() );
        scoped_lock lk( _mutex );
        _cursors[cursor->getId()] = cursor;
        _shardedTotal++;
    }
    void CursorCache::remove( long long id ) {
        verify( id );
        scoped_lock lk( _mutex );
        _cursors.erase( id );
    }
    
    void CursorCache::removeRef( long long id ) {
        verify( id );
        scoped_lock lk( _mutex );
        _refs.erase( id );
        _refsNS.erase( id );
    }

    void CursorCache::storeRef(const std::string& server, long long id, const std::string& ns) {
        LOG(_myLogLevel) << "CursorCache::storeRef server: " << server << " id: " << id << endl;
        verify( id );
        scoped_lock lk( _mutex );
        _refs[id] = server;
        _refsNS[id] = ns;
    }

    string CursorCache::getRef( long long id ) const {
        verify( id );
        scoped_lock lk( _mutex );
        MapNormal::const_iterator i = _refs.find( id );

        LOG(_myLogLevel) << "CursorCache::getRef id: " << id << " out: " << ( i == _refs.end() ? " NONE " : i->second ) << endl;

        if ( i == _refs.end() )
            return "";
        return i->second;
    }

    std::string CursorCache::getRefNS(long long id) const {
        verify(id);
        scoped_lock lk(_mutex);
        MapNormal::const_iterator i = _refsNS.find(id);

        LOG(_myLogLevel) << "CursorCache::getRefNs id: " << id
                << " out: " << ( i == _refsNS.end() ? " NONE " : i->second ) << std::endl;

        if ( i == _refsNS.end() )
            return "";
        return i->second;
    }


    long long CursorCache::genId() {
        while ( true ) {
            scoped_lock lk( _mutex );

            long long x = Listener::getElapsedTimeMillis() << 32;
            x |= _random.nextInt32();

            if ( x == 0 )
                continue;

            if ( x < 0 )
                x *= -1;

            MapSharded::iterator i = _cursors.find( x );
            if ( i != _cursors.end() )
                continue;

            MapNormal::iterator j = _refs.find( x );
            if ( j != _refs.end() )
                continue;

            return x;
        }
    }

    void CursorCache::gotKillCursors(Message& m ) {
        int *x = (int *) m.singleData()->_data;
        x++; // reserved
        int n = *x++;

        if ( n > 2000 ) {
            ( n < 30000 ? warning() : error() ) << "receivedKillCursors, n=" << n << endl;
        }


        uassert( 13286 , "sent 0 cursors to kill" , n >= 1 );
        uassert( 13287 , "too many cursors to kill" , n < 30000 );

        long long * cursors = (long long *)x;
        ClientBasic* client = ClientBasic::getCurrent();
        AuthorizationSession* authSession = client->getAuthorizationSession();
        for ( int i=0; i<n; i++ ) {
            long long id = cursors[i];
            LOG(_myLogLevel) << "CursorCache::gotKillCursors id: " << id << endl;

            if ( ! id ) {
                warning() << " got cursor id of 0 to kill" << endl;
                continue;
            }

            string server;
            {
                scoped_lock lk( _mutex );

                MapSharded::iterator i = _cursors.find( id );
                if ( i != _cursors.end() ) {
                    const bool isAuthorized = authSession->checkAuthorization(
                            i->second->getNS(), ActionType::killCursors);
                    audit::logKillCursorsAuthzCheck(
                            client,
                            NamespaceString(i->second->getNS()),
                            id,
                            isAuthorized ? ErrorCodes::OK : ErrorCodes::Unauthorized);
                    if (isAuthorized) {
                        _cursors.erase( i );
                    }
                    continue;
                }

                MapNormal::iterator refsIt = _refs.find(id);
                MapNormal::iterator refsNSIt = _refsNS.find(id);
                if (refsIt == _refs.end()) {
                    warning() << "can't find cursor: " << id << endl;
                    continue;
                }
                verify(refsNSIt != _refsNS.end());
                const bool isAuthorized = authSession->checkAuthorization(
                        refsNSIt->second, ActionType::killCursors);
                audit::logKillCursorsAuthzCheck(
                        client,
                        NamespaceString(refsNSIt->second),
                        id,
                        isAuthorized ? ErrorCodes::OK : ErrorCodes::Unauthorized);
                if (!isAuthorized) {
                    continue;
                }
                server = refsIt->second;
                _refs.erase(refsIt);
                _refsNS.erase(refsNSIt);
            }

            LOG(_myLogLevel) << "CursorCache::found gotKillCursors id: " << id << " server: " << server << endl;

            verify( server.size() );
            ScopedDbConnection conn(server);
            conn->killCursor( id );
            conn.done();
        }
    }

    void CursorCache::appendInfo( BSONObjBuilder& result ) const {
        scoped_lock lk( _mutex );
        result.append( "sharded" , (int)_cursors.size() );
        result.appendNumber( "shardedEver" , _shardedTotal );
        result.append( "refs" , (int)_refs.size() );
        result.append( "totalOpen" , (int)(_cursors.size() + _refs.size() ) );
    }

    void CursorCache::doTimeouts() {
        long long now = Listener::getElapsedTimeMillis();
        scoped_lock lk( _mutex );
        for ( MapSharded::iterator i=_cursors.begin(); i!=_cursors.end(); ++i ) {
            // Note: cursors with no timeout will always have an idleTime of 0
            long long idleFor = i->second->idleTime( now );
            if ( idleFor < TIMEOUT ) {
                continue;
            }
            log() << "killing old cursor " << i->second->getId() << " idle for: " << idleFor << "ms" << endl; // TODO: make LOG(1)
            _cursors.erase( i );
            i = _cursors.begin(); // possible 2nd entry will get skipped, will get on next pass
            if ( i == _cursors.end() )
                break;
        }
    }

    CursorCache cursorCache;

    const int CursorCache::_myLogLevel = 3;

    class CursorTimeoutTask : public task::Task {
    public:
        virtual string name() const { return "cursorTimeout"; }
        virtual void doWork() {
            cursorCache.doTimeouts();
        }
    };

    void CursorCache::startTimeoutThread() {
        task::repeat( new CursorTimeoutTask , 4000 );
    }

    class CmdCursorInfo : public Command {
    public:
        CmdCursorInfo() : Command( "cursorInfo", true ) {}
        virtual bool slaveOk() const { return true; }
        virtual void help( stringstream& help ) const {
            help << " example: { cursorInfo : 1 }";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::cursorInfo);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }
        virtual LockType locktype() const { return NONE; }
        bool run(const string&, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            cursorCache.appendInfo( result );
            if ( jsobj["setTimeout"].isNumber() )
                CursorCache::TIMEOUT = jsobj["setTimeout"].numberLong();
            return true;
        }
    } cmdCursorInfo;

}
