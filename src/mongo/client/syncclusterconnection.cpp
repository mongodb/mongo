// syncclusterconnection.cpp
/*
 *    Copyright 2010 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */


#include "pch.h"

#include "mongo/client/syncclusterconnection.h"

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/dbmessage.h"

// error codes 8000-8009

namespace mongo {

    SyncClusterConnection::SyncClusterConnection( const list<HostAndPort> & L, double socketTimeout) : _mutex("SyncClusterConnection"), _socketTimeout( socketTimeout ) {
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

    SyncClusterConnection::SyncClusterConnection( string commaSeperated, double socketTimeout)  : _mutex("SyncClusterConnection"), _socketTimeout( socketTimeout ) {
        _address = commaSeperated;
        string::size_type idx;
        while ( ( idx = commaSeperated.find( ',' ) ) != string::npos ) {
            string h = commaSeperated.substr( 0 , idx );
            commaSeperated = commaSeperated.substr( idx + 1 );
            _connect( h );
        }
        _connect( commaSeperated );
        uassert( 8004 ,  "SyncClusterConnection needs 3 servers" , _conns.size() == 3 );
    }

    SyncClusterConnection::SyncClusterConnection( string a , string b , string c, double socketTimeout)  : _mutex("SyncClusterConnection"), _socketTimeout( socketTimeout ) {
        _address = a + "," + b + "," + c;
        // connect to all even if not working
        _connect( a );
        _connect( b );
        _connect( c );
    }

    SyncClusterConnection::SyncClusterConnection( SyncClusterConnection& prev, double socketTimeout) : _mutex("SyncClusterConnection"), _socketTimeout( socketTimeout ) {
        verify(0);
    }

    SyncClusterConnection::~SyncClusterConnection() {
        for ( size_t i=0; i<_conns.size(); i++ )
            delete _conns[i];
        _conns.clear();
    }

    bool SyncClusterConnection::prepare( string& errmsg ) {
        _lastErrors.clear();
        return fsync( errmsg );
    }

    bool SyncClusterConnection::fsync( string& errmsg ) {
        bool ok = true;
        errmsg = "";
        for ( size_t i=0; i<_conns.size(); i++ ) {
            BSONObj res;
            try {
                if ( _conns[i]->simpleCommand( "admin" , &res , "fsync" ) )
                    continue;
            }
            catch ( DBException& e ) {
                errmsg += e.toString();
            }
            catch ( std::exception& e ) {
                errmsg += e.what();
            }
            catch ( ... ) {
                warning() << "unknown exception in SyncClusterConnection::fsync" << endl;
            }
            ok = false;
            errmsg += " " + _conns[i]->toString() + ":" + res.toString();
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
            if ( res["ok"].trueValue() && (res["fsyncFiles"].numberInt() > 0 || res.hasElement("waited")))
                continue;
            ok = false;
            err << _conns[i]->toString() << ": " << res << " " << errors[i];
        }

        if ( ok )
            return;
        throw UserException( 8001 , (string)"SyncClusterConnection write op failed: " + err.str() );
    }

    BSONObj SyncClusterConnection::getLastErrorDetailed(bool fsync, bool j, int w, int wtimeout) {
        if ( _lastErrors.size() )
            return _lastErrors[0];
        return DBClientBase::getLastErrorDetailed(fsync,j,w,wtimeout);
    }

    void SyncClusterConnection::_connect( string host ) {
        log() << "SyncClusterConnection connecting to [" << host << "]" << endl;
        DBClientConnection * c = new DBClientConnection( true );
        c->setSoTimeout( _socketTimeout );
        string errmsg;
        if ( ! c->connect( host , errmsg ) )
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
                    throw UserException( 13104 , (string)"SyncClusterConnection::findOne prepare failed: " + errmsg );

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

    bool SyncClusterConnection::auth(const string &dbname, const string &username, const string &password_text, string& errmsg, bool digestPassword, Auth::Level* level) {
        for (vector<DBClientConnection*>::iterator it = _conns.begin(); it < _conns.end(); it++) {
            massert( 15848, "sync cluster of sync clusters?", (*it)->type() != ConnectionString::SYNC);

            if (!(*it)->auth(dbname, username, password_text, errmsg, digestPassword, level)) {
                return false;
            }
        }
        return true;
    }

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
        auto_ptr<DBClientCursor> cursor = _queryOnActive( dbname + ".$cmd" , cmd , 1 , 0 , 0 , options , 0 );
        if ( cursor->more() )
            info = cursor->next().copy();
        else
            info = BSONObj();
        return isOk( info );
    }

    auto_ptr<DBClientCursor> SyncClusterConnection::_queryOnActive(const string &ns, Query query, int nToReturn, int nToSkip,
            const BSONObj *fieldsToReturn, int queryOptions, int batchSize ) {

        for ( size_t i=0; i<_conns.size(); i++ ) {
            try {
                auto_ptr<DBClientCursor> cursor =
                    _conns[i]->query( ns , query , nToReturn , nToSkip , fieldsToReturn , queryOptions , batchSize );
                if ( cursor.get() )
                    return cursor;
                log() << "query failed to: " << _conns[i]->toString() << " no data" << endl;
            }
            catch ( std::exception& e ) {
                log() << "query failed to: " << _conns[i]->toString() << " exception: " << e.what() << endl;
            }
            catch ( ... ) {
                log() << "query failed to: " << _conns[i]->toString() << " exception" << endl;
            }
        }
        throw UserException( 8002 , "all servers down!" );
    }

    auto_ptr<DBClientCursor> SyncClusterConnection::getMore( const string &ns, long long cursorId, int nToReturn, int options ) {
        uassert( 10022 , "SyncClusterConnection::getMore not supported yet" , 0);
        auto_ptr<DBClientCursor> c;
        return c;
    }

    void SyncClusterConnection::insert( const string &ns, BSONObj obj , int flags) {

        uassert( 13119 , (string)"SyncClusterConnection::insert obj has to have an _id: " + obj.jsonString() ,
                 ns.find( ".system.indexes" ) != string::npos || obj["_id"].type() );

        string errmsg;
        if ( ! prepare( errmsg ) )
            throw UserException( 8003 , (string)"SyncClusterConnection::insert prepare failed: " + errmsg );

        for ( size_t i=0; i<_conns.size(); i++ ) {
            _conns[i]->insert( ns , obj , flags);
        }

        _checkLast();
    }

    void SyncClusterConnection::insert( const string &ns, const vector< BSONObj >& v , int flags) {
        uassert( 10023 , "SyncClusterConnection bulk insert not implemented" , 0);
    }

    void SyncClusterConnection::remove( const string &ns , Query query, bool justOne ) {
        string errmsg;
        if ( ! prepare( errmsg ) )
            throw UserException( 8020 , (string)"SyncClusterConnection::remove prepare failed: " + errmsg );

        for ( size_t i=0; i<_conns.size(); i++ ) {
            _conns[i]->remove( ns , query , justOne );
        }

        _checkLast();
    }

    void SyncClusterConnection::update( const string &ns , Query query , BSONObj obj , bool upsert , bool multi ) {

        if ( upsert ) {
            uassert( 13120 , "SyncClusterConnection::update upsert query needs _id" , query.obj["_id"].type() );
        }

        if ( _writeConcern ) {
            string errmsg;
            if ( ! prepare( errmsg ) )
                throw UserException( 8005 , (string)"SyncClusterConnection::udpate prepare failed: " + errmsg );
        }

        for ( size_t i = 0; i < _conns.size(); i++ ) {
            try {
                _conns[i]->update( ns , query , obj , upsert , multi );
            }
            catch ( std::exception& e ) {
                if ( _writeConcern )
                    throw e;
            }
        }

        if ( _writeConcern ) {
            _checkLast();
            verify( _lastErrors.size() > 1 );

            int a = _lastErrors[0]["n"].numberInt();
            for ( unsigned i=1; i<_lastErrors.size(); i++ ) {
                int b = _lastErrors[i]["n"].numberInt();
                if ( a == b )
                    continue;

                throw UpdateNotTheSame( 8017 , 
                                        str::stream() 
                                        << "update not consistent " 
                                        << " ns: " << ns
                                        << " query: " << query.toString()
                                        << " update: " << obj
                                        << " gle1: " << _lastErrors[0]
                                        << " gle2: " << _lastErrors[i] ,
                                        _connAddresses , _lastErrors );
            }
        }
    }

    string SyncClusterConnection::_toString() const {
        stringstream ss;
        ss << "SyncClusterConnection [" << _address << "]";
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
        throw UserException( 8008 , "all servers down!" );
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
            scoped_lock lk(_mutex);
            map<string,int>::iterator i = _lockTypes.find( name );
            if ( i != _lockTypes.end() )
                return i->second;
        }

        BSONObj info;
        uassert( 13053 , str::stream() << "help failed: " << info , _commandOnActive( "admin" , BSON( name << "1" << "help" << 1 ) , info ) );

        int lockType = info["lockType"].numberInt();

        scoped_lock lk(_mutex);
        _lockTypes[name] = lockType;
        return lockType;
    }

    void SyncClusterConnection::killCursor( long long cursorID ) {
        // should never need to do this
        verify(0);
    }

    void SyncClusterConnection::setAllSoTimeouts( double socketTimeout ){
        _socketTimeout = socketTimeout;
        for ( size_t i=0; i<_conns.size(); i++ )

            if( _conns[i] ) _conns[i]->setSoTimeout( socketTimeout );
    }

}
