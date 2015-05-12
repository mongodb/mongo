// syncclusterconnection.cpp
/*
 *    Copyright 2010 10gen Inc.
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


#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/syncclusterconnection.h"

#include "mongo/client/dbclientcursor.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/log.h"

// error codes 8000-8009

namespace mongo {

    using std::auto_ptr;
    using std::endl;
    using std::list;
    using std::map;
    using std::string;
    using std::stringstream;
    using std::vector;

    SyncClusterConnection::SyncClusterConnection( const list<HostAndPort> & L, double socketTimeout)
        : _socketTimeout(socketTimeout) {

        {
            stringstream s;
            int n=0;
            for( list<HostAndPort>::const_iterator i = L.begin(); i != L.end(); i++ ) {
                if( ++n > 1 ) s << ',';
                s << i->toString();
            }
            _address = s.str();
        }
        for( list<HostAndPort>::const_iterator i = L.begin(); i != L.end(); i++ )
            _connect( i->toString() );
    }

    SyncClusterConnection::SyncClusterConnection( string commaSeparated, double socketTimeout) :
        _socketTimeout( socketTimeout ) {

        _address = commaSeparated;
        string::size_type idx;
        while ( ( idx = commaSeparated.find( ',' ) ) != string::npos ) {
            string h = commaSeparated.substr( 0 , idx );
            commaSeparated = commaSeparated.substr( idx + 1 );
            _connect( h );
        }
        _connect( commaSeparated );
        uassert( 8004 ,  "SyncClusterConnection needs 3 servers" , _conns.size() == 3 );
    }

    SyncClusterConnection::SyncClusterConnection(
            const std::string& a,
            const std::string& b,
            const std::string& c,
            double socketTimeout) : _socketTimeout( socketTimeout ) {

        _address = a + "," + b + "," + c;
        // connect to all even if not working
        _connect( a );
        _connect( b );
        _connect( c );
    }

    SyncClusterConnection::SyncClusterConnection( SyncClusterConnection& prev, double socketTimeout)
        : _socketTimeout(socketTimeout) {
        verify(0);
    }

    SyncClusterConnection::~SyncClusterConnection() {
        for ( size_t i=0; i<_conns.size(); i++ )
            delete _conns[i];
        _conns.clear();
    }

    bool SyncClusterConnection::prepare(string& errmsg) {
        _lastErrors.clear();

        bool ok = true;
        errmsg = "";

        for (size_t i = 0; i < _conns.size(); i++) {
            string singleErr;
            try {
                _conns[i]->simpleCommand("admin", NULL, "resetError");
                singleErr = _conns[i]->getLastError(true);

                if (singleErr.size() == 0)
                    continue;

            }
            catch (DBException& e) {
                singleErr = e.toString();
            }
            ok = false;
            errmsg += " " + _conns[i]->toString() + ":" + singleErr;
        }

        return ok;
    }

    void SyncClusterConnection::_checkLast() {
        _lastErrors.clear();
        vector<string> errors;

        for ( size_t i=0; i<_conns.size(); i++ ) {
            BSONObj res;
            string err;
            try {
                if ( ! _conns[i]->runCommand( "admin" , BSON( "getlasterror" << 1 << "fsync" << 1 ) , res ) )
                    err = "cmd failed: ";
            }
            catch ( std::exception& e ) {
                err += e.what();
            }
            catch ( ... ) {
                err += "unknown failure";
            }
            _lastErrors.push_back( res.getOwned() );
            errors.push_back( err );
        }

        verify( _lastErrors.size() == errors.size() && _lastErrors.size() == _conns.size() );

        stringstream err;
        bool ok = true;

        for ( size_t i = 0; i<_conns.size(); i++ ) {
            BSONObj res = _lastErrors[i];
            if ( res["ok"].trueValue() && (res["fsyncFiles"].numberInt() > 0 ||
                                           res.hasElement("waited") ||
                                           res["syncMillis"].numberInt() >= 0 ) )
                continue;
            ok = false;
            err << _conns[i]->toString() << ": " << res << " " << errors[i];
        }

        if ( ok )
            return;
        throw UserException( 8001 , (string)"SyncClusterConnection write op failed: " + err.str() );
    }

    BSONObj SyncClusterConnection::getLastErrorDetailed(bool fsync, bool j, int w, int wtimeout) {
        return getLastErrorDetailed("admin", fsync, j, w, wtimeout);
    }

    BSONObj SyncClusterConnection::getLastErrorDetailed(const std::string& db,
                                                        bool fsync,
                                                        bool j,
                                                        int w,
                                                        int wtimeout) {
        if ( _lastErrors.size() )
            return _lastErrors[0];
        return DBClientBase::getLastErrorDetailed(db,fsync,j,w,wtimeout);
    }

    void SyncClusterConnection::_connect( const std::string& host ) {
        log() << "SyncClusterConnection connecting to [" << host << "]" << endl;
        DBClientConnection * c = new DBClientConnection( true );
        c->setRunCommandHook(_runCommandHook);
        c->setPostRunCommandHook(_postRunCommandHook);
        c->setSoTimeout( _socketTimeout );
        string errmsg;
        if ( ! c->connect( HostAndPort(host), errmsg ) )
            log() << "SyncClusterConnection connect fail to: " << host << " errmsg: " << errmsg << endl;
        _connAddresses.push_back( host );
        _conns.push_back( c );
    }

    bool SyncClusterConnection::callRead( Message& toSend , Message& response ) {
        // TODO: need to save state of which one to go back to somehow...
        return _conns[0]->callRead( toSend , response );
    }

    BSONObj SyncClusterConnection::findOne(const string &ns, const Query& query, const BSONObj *fieldsToReturn, int queryOptions) {

        if ( ns.find( ".$cmd" ) != string::npos ) {
            string cmdName = query.obj.firstElementFieldName();

            int lockType = _lockType( cmdName );

            if ( lockType > 0 ) { // write $cmd
                string errmsg;
                if ( ! prepare( errmsg ) )
                    throw UserException( PrepareConfigsFailedCode , (string)"SyncClusterConnection::findOne prepare failed: " + errmsg );

                vector<BSONObj> all;
                for ( size_t i=0; i<_conns.size(); i++ ) {
                    all.push_back( _conns[i]->findOne( ns , query , 0 , queryOptions ).getOwned() );
                }

                _checkLast();
                
                for ( size_t i=0; i<all.size(); i++ ) {
                    BSONObj temp = all[i];
                    if ( isOk( temp ) )
                        continue;
                    stringstream ss;
                    ss << "write $cmd failed on a node: " << temp.jsonString();
                    ss << " " << _conns[i]->toString();
                    ss << " ns: " << ns;
                    ss << " cmd: " << query.toString();
                    throw UserException( 13105 , ss.str() );
                }

                return all[0];
            }
        }

        return DBClientBase::findOne( ns , query , fieldsToReturn , queryOptions );
    }

    void SyncClusterConnection::_auth(const BSONObj& params) {
        // A SCC is authenticated if any connection has been authenticated
        // Credentials are stored in the auto-reconnect connections.

        bool authedOnce = false;
        vector<string> errors;

        for ( vector<DBClientConnection*>::iterator it = _conns.begin(); it < _conns.end(); ++it ) {

            massert( 15848, "sync cluster of sync clusters?",
                            (*it)->type() != ConnectionString::SYNC );

            // Authenticate or collect the error message
            string lastErrmsg;
            bool authed;
            try {
                // Auth errors can manifest either as exceptions or as false results
                // TODO: Make this better
                (*it)->auth(params);
                authed = true;
            }
            catch ( const DBException& e ) {
                // auth will be retried on reconnect
                lastErrmsg = e.what();
                authed = false;
            }

            if ( ! authed ) {

                // Since we're using auto-reconnect connections, we're sure the auth info has been
                // stored if needed for later

                lastErrmsg = str::stream() << "auth error on " << (*it)->getServerAddress()
                                           << causedBy( lastErrmsg );

                LOG(1) << lastErrmsg << endl;
                errors.push_back( lastErrmsg );
            }

            authedOnce = authedOnce || authed;
        }

        if( authedOnce ) return;

        // Assemble the error message
        str::stream errStream;
        for( vector<string>::iterator it = errors.begin(); it != errors.end(); ++it ){
            if( it != errors.begin() ) errStream << " ::and:: ";
            errStream << *it;
        }

        uasserted(ErrorCodes::AuthenticationFailed, errStream);
    }

    // TODO: logout is required for use of this class outside of a cluster environment

    auto_ptr<DBClientCursor> SyncClusterConnection::query(const string &ns, Query query, int nToReturn, int nToSkip,
            const BSONObj *fieldsToReturn, int queryOptions, int batchSize ) {
        _lastErrors.clear();
        if ( ns.find( ".$cmd" ) != string::npos ) {
            string cmdName = query.obj.firstElementFieldName();
            int lockType = _lockType( cmdName );
            uassert( 13054 , (string)"write $cmd not supported in SyncClusterConnection::query for:" + cmdName , lockType <= 0 );
        }

        return _queryOnActive( ns , query , nToReturn , nToSkip , fieldsToReturn , queryOptions , batchSize );
    }

    bool SyncClusterConnection::_commandOnActive(const string &dbname, const BSONObj& cmd, BSONObj &info, int options ) {
        auto_ptr<DBClientCursor> cursor = _queryOnActive(dbname + ".$cmd", cmd, 1, 0, 0, options, 0);
        if ( cursor->more() )
            info = cursor->next().copy();
        else
            info = BSONObj();
        return isOk( info );
    }

    void SyncClusterConnection::attachQueryHandler( QueryHandler* handler ) {
        _customQueryHandler.reset( handler );
    }

    auto_ptr<DBClientCursor> SyncClusterConnection::_queryOnActive(const string &ns, Query query, int nToReturn, int nToSkip,
            const BSONObj *fieldsToReturn, int queryOptions, int batchSize ) {

        if ( _customQueryHandler && _customQueryHandler->canHandleQuery( ns, query ) ) {

            LOG( 2 ) << "custom query handler used for query on " << ns << ": "
                     << query.toString() << endl;

            return _customQueryHandler->handleQuery( _connAddresses,
                                                     ns,
                                                     query,
                                                     nToReturn,
                                                     nToSkip,
                                                     fieldsToReturn,
                                                     queryOptions,
                                                     batchSize );
        }

        for ( size_t i=0; i<_conns.size(); i++ ) {
            try {
                auto_ptr<DBClientCursor> cursor =
                    _conns[i]->query( ns , query , nToReturn , nToSkip , fieldsToReturn , queryOptions , batchSize );
                if ( cursor.get() )
                    return cursor;

                log() << "query on " << ns << ": " << query.toString() << " failed to: "
                      << _conns[i]->toString() << " no data" << endl;
            }
            catch ( std::exception& e ) {

                log() << "query on " << ns << ": " << query.toString() << " failed to: "
                      << _conns[i]->toString() << " exception: " << e.what() << endl;
            }
            catch ( ... ) {

                log() << "query on " << ns << ": " << query.toString() << " failed to: "
                      << _conns[i]->toString() << " exception" << endl;
            }
        }
        throw UserException( 8002 , str::stream() << "all servers down/unreachable when querying: " << _address );
    }

    auto_ptr<DBClientCursor> SyncClusterConnection::getMore( const string &ns, long long cursorId, int nToReturn, int options ) {
        uassert( 10022 , "SyncClusterConnection::getMore not supported yet" , 0);
        auto_ptr<DBClientCursor> c;
        return c;
    }

    void SyncClusterConnection::insert( const string &ns, BSONObj obj , int flags) {

        uassert(13119,
                str::stream() << "SyncClusterConnection::insert obj has to have an _id: " << obj,
                nsToCollectionSubstring(ns) == "system.indexes" || obj["_id"].type());

        string errmsg;
        if ( ! prepare( errmsg ) )
            throw UserException( 8003 , (string)"SyncClusterConnection::insert prepare failed: " + errmsg );

        for ( size_t i=0; i<_conns.size(); i++ ) {
            _conns[i]->insert( ns , obj , flags);
        }

        _checkLast();
    }

    void SyncClusterConnection::insert( const string &ns, const vector< BSONObj >& v , int flags) {
        if (v.size() == 1){
            insert(ns, v[0], flags);
            return;
        }

        for (vector<BSONObj>::const_iterator it = v.begin(); it != v.end(); ++it ) {
            BSONObj obj = *it;
            if ( obj["_id"].type() == EOO ) {
                string assertMsg = "SyncClusterConnection::insert (batched) obj misses an _id: ";
                uasserted( 16743, assertMsg + obj.jsonString() );
            }
        }

        // fsync all connections before starting the batch.
        string errmsg;
        if ( ! prepare( errmsg ) ) {
            string assertMsg = "SyncClusterConnection::insert (batched) prepare failed: ";
            throw UserException( 16744, assertMsg + errmsg );
        }

        // We still want one getlasterror per document, even if they're batched.
        for ( size_t i=0; i<_conns.size(); i++ ) {
            for ( vector<BSONObj>::const_iterator it = v.begin(); it != v.end(); ++it ) {
                _conns[i]->insert( ns, *it, flags );
                _conns[i]->getLastErrorDetailed();
            }
        }

        // We issue a final getlasterror, but this time with an fsync.
        _checkLast();
    }

    void SyncClusterConnection::remove( const string &ns , Query query, int flags ) {
        string errmsg;
        if ( ! prepare( errmsg ) )
            throw UserException( 8020 , (string)"SyncClusterConnection::remove prepare failed: " + errmsg );

        for ( size_t i=0; i<_conns.size(); i++ ) {
            _conns[i]->remove( ns , query , flags );
        }

        _checkLast();
    }

    void SyncClusterConnection::update(const string &ns, Query query, BSONObj obj, int flags) {
        if ( flags & UpdateOption_Upsert ) {
            uassert(13120,
                    "SyncClusterConnection::update upsert query needs _id",
                    query.obj["_id"].type());
        }

        string errmsg;
        if (!prepare(errmsg)) {
            throw UserException(8005,
                                str::stream() << "SyncClusterConnection::update prepare failed: "
                                              << errmsg);
        }

        for (size_t i = 0; i < _conns.size(); i++) {
            _conns[i]->update(ns, query, obj, flags);
        }

        _checkLast();
        invariant(_lastErrors.size() > 1);

        const int a = _lastErrors[0]["n"].numberInt();

        for (unsigned i = 1; i < _lastErrors.size(); i++) {
            int b = _lastErrors[i]["n"].numberInt();

            if (a == b)
                continue;

            throw UpdateNotTheSame(8017,
                                   str::stream() << "update not consistent "
                                                 << " ns: " << ns
                                                 << " query: " << query.toString()
                                                 << " update: " << obj
                                                 << " gle1: " << _lastErrors[0]
                                                 << " gle2: " << _lastErrors[i],
                                   _connAddresses,
                                   _lastErrors);
        }
    }

    string SyncClusterConnection::_toString() const {
        stringstream ss;
        ss << "SyncClusterConnection ";
        ss << " [";
        for ( size_t i = 0; i < _conns.size(); i++ ) {
            if ( i != 0 ) ss << ",";
            if ( _conns[i] ) {
                ss << _conns[i]->toString();
            }
            else {
                ss << "(no conn)";
            }
        }
        ss << "]";
        return ss.str();
    }

    bool SyncClusterConnection::call( Message &toSend, Message &response, bool assertOk , string * actualServer ) {
        uassert( 8006 , "SyncClusterConnection::call can only be used directly for dbQuery" ,
                 toSend.operation() == dbQuery );

        DbMessage d( toSend );
        uassert( 8007 , "SyncClusterConnection::call can't handle $cmd" , strstr( d.getns(), "$cmd" ) == 0 );

        for ( size_t i=0; i<_conns.size(); i++ ) {
            try {
                bool ok = _conns[i]->call( toSend , response , assertOk );
                if ( ok ) {
                    if ( actualServer )
                        *actualServer = _connAddresses[i];
                    return ok;
                }
                log() << "call failed to: " << _conns[i]->toString() << " no data" << endl;
            }
            catch ( ... ) {
                log() << "call failed to: " << _conns[i]->toString() << " exception" << endl;
            }
        }
        throw UserException( 8008 , str::stream() << "all servers down/unreachable: " << _address );
    }

    void SyncClusterConnection::say( Message &toSend, bool isRetry , string * actualServer ) {
        string errmsg;
        if ( ! prepare( errmsg ) )
            throw UserException( 13397 , (string)"SyncClusterConnection::say prepare failed: " + errmsg );

        for ( size_t i=0; i<_conns.size(); i++ ) {
            _conns[i]->say( toSend );
        }

        // TODO: should we set actualServer??

        _checkLast();
    }

    void SyncClusterConnection::sayPiggyBack( Message &toSend ) {
        verify(0);
    }

    int SyncClusterConnection::_lockType( const string& name ) {
        {
            boost::lock_guard<boost::mutex> lk(_mutex);
            map<string,int>::iterator i = _lockTypes.find( name );
            if ( i != _lockTypes.end() )
                return i->second;
        }

        BSONObj info;
        uassert( 13053 , str::stream() << "help failed: " << info , _commandOnActive( "admin" , BSON( name << "1" << "help" << 1 ) , info ) );

        int lockType = info["lockType"].numberInt();

        boost::lock_guard<boost::mutex> lk(_mutex);
        _lockTypes[name] = lockType;
        return lockType;
    }

    void SyncClusterConnection::killCursor( long long cursorID ) {
        // should never need to do this
        verify(0);
    }

    // A SCC should be reused only if all the existing connections haven't been broken in the
    // background.
    // Note: an SCC may have missing connections if a config server is temporarily offline,
    // but reading from the others is still allowed.
    bool SyncClusterConnection::isStillConnected() {
        for ( size_t i = 0; i < _conns.size(); i++ ) {
            if ( _conns[i] && !_conns[i]->isStillConnected() ) return false;

        }
        return true;
    }

    void SyncClusterConnection::setAllSoTimeouts( double socketTimeout ){
        _socketTimeout = socketTimeout;
        for ( size_t i=0; i<_conns.size(); i++ )

            if( _conns[i] ) _conns[i]->setSoTimeout( socketTimeout );
    }

    void SyncClusterConnection::setRunCommandHook(DBClientWithCommands::RunCommandHookFunc func) {
        // Set the hooks in both our sub-connections and in ourselves.
        for (size_t i = 0; i < _conns.size(); ++i) {
            if (_conns[i]) { 
                _conns[i]->setRunCommandHook(func);
            }
        }
        _runCommandHook = func;
    }

    void SyncClusterConnection::setPostRunCommandHook
    (DBClientWithCommands::PostRunCommandHookFunc func) {
        // Set the hooks in both our sub-connections and in ourselves.
        for (size_t i = 0; i < _conns.size(); ++i) {
            if (_conns[i]) { 
                _conns[i]->setPostRunCommandHook(func);
            }
        }
        _postRunCommandHook = func;
    }
}
