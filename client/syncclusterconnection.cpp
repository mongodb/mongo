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


#include "stdafx.h"
#include "syncclusterconnection.h"

// error codes 8000-8009

namespace mongo {
    
    SyncClusterConnection::SyncClusterConnection( string commaSeperated ){
        string::size_type idx;
        while ( ( idx = commaSeperated.find( ',' ) ) != string::npos ){
            string h = commaSeperated.substr( 0 , idx );
            commaSeperated = commaSeperated.substr( idx + 1 );
            _connect( h );
        }
        _connect( commaSeperated );
        uassert( 8004 ,  "SyncClusterConnection needs 3 servers" , _conns.size() == 3 );
    }

    SyncClusterConnection::SyncClusterConnection( string a , string b , string c ){
        // connect to all even if not working
        _connect( a );
        _connect( b );
        _connect( c );
    }

    SyncClusterConnection::~SyncClusterConnection(){
        for ( size_t i=0; i<_conns.size(); i++ )
            delete _conns[i];
        _conns.clear();
    }

    bool SyncClusterConnection::prepare( string& errmsg ){
        return fsync( errmsg );
    }
    
    bool SyncClusterConnection::fsync( string& errmsg ){
        bool ok = true;
        errmsg = "";
        for ( size_t i=0; i<_conns.size(); i++ ){
            BSONObj res;
            try {
                if ( _conns[i]->simpleCommand( "admin" , 0 , "fsync" ) )
                    continue;
            }
            catch ( std::exception& e ){
                errmsg += e.what();
            }
            catch ( ... ){
            }
            ok = false;
            errmsg += _conns[i]->toString() + ":" + res.toString();
        }
        return ok;
    }

    void SyncClusterConnection::_checkLast(){
        vector<BSONObj> all;
        vector<string> errors;

        for ( size_t i=0; i<_conns.size(); i++ ){
            BSONObj res;
            string err;
            try {
                if ( ! _conns[i]->runCommand( "admin" , BSON( "getlasterror" << 1 << "fsync" << 1 ) , res ) )
                    err = "cmd failed: ";
            }
            catch ( std::exception& e ){
                err += e.what();
            }
            catch ( ... ){
                err += "unknown failure";
            }
            all.push_back( res );
            errors.push_back( err );
        }
        
        assert( all.size() == errors.size() && all.size() == _conns.size() );
        
        stringstream err;
        bool ok = true;
        
        for ( size_t i = 0; i<_conns.size(); i++ ){
            BSONObj res = all[i];
            if ( res["ok"].trueValue() && res["fsyncFiles"].numberInt() > 0 )
                continue;
            ok = false;
            err << _conns[i]->toString() << ": " << res << " " << errors[i];
        }

        if ( ok )
            return;
        throw UserException( 8001 , (string)"SyncClusterConnection write op failed: " + err.str() );
    }

    void SyncClusterConnection::_connect( string host ){
        log() << "SyncClusterConnection connecting to: " << host << endl;
        DBClientConnection * c = new DBClientConnection( true );
        string errmsg;
        if ( ! c->connect( host , errmsg ) )
            log() << "SyncClusterConnection connect fail to: " << host << " errmsg: " << errmsg << endl;
        _conns.push_back( c );
    }

    auto_ptr<DBClientCursor> SyncClusterConnection::query(const string &ns, Query query, int nToReturn, int nToSkip,
                                                          const BSONObj *fieldsToReturn, int queryOptions, int batchSize ){ 

        if ( ns.find( ".$cmd" ) != string::npos ){
            string cmdName = query.obj.firstElement().fieldName();

            int lockType = 0;
            
            map<string,int>::iterator i = _lockTypes.find( cmdName );
            if ( i == _lockTypes.end() ){
                BSONObj info;
                uassert( 13053 , "help failed" , _commandOnActive( "admin" , BSON( cmdName << "1" << "help" << 1 ) , info ) );
                lockType = info["lockType"].numberInt();
                _lockTypes[cmdName] = lockType;
            }
            else {
                lockType = i->second;
            }
            
            uassert( 13054 , "write $cmd not supported in SyncClusterConnection" , lockType <= 0 );
        }

        return _queryOnActive( ns , query , nToReturn , nToSkip , fieldsToReturn , queryOptions , batchSize );
    }

    bool SyncClusterConnection::_commandOnActive(const string &dbname, const BSONObj& cmd, BSONObj &info, int options ){
        auto_ptr<DBClientCursor> cursor = _queryOnActive( dbname + ".$cmd" , cmd , 1 , 0 , 0 , options , 0 );
        if ( cursor->more() )
            info = cursor->next().copy();
        else
            info = BSONObj();
        return isOk( info );
    }
    
    auto_ptr<DBClientCursor> SyncClusterConnection::_queryOnActive(const string &ns, Query query, int nToReturn, int nToSkip,
                                                                   const BSONObj *fieldsToReturn, int queryOptions, int batchSize ){ 
        
        for ( size_t i=0; i<_conns.size(); i++ ){
            try {
                auto_ptr<DBClientCursor> cursor = 
                    _conns[i]->query( ns , query , nToReturn , nToSkip , fieldsToReturn , queryOptions , batchSize );
                if ( cursor.get() )
                    return cursor;
                log() << "query failed to: " << _conns[i]->toString() << " no data" << endl;
            }
            catch ( ... ){
                log() << "query failed to: " << _conns[i]->toString() << " exception" << endl;
            }
        }
        throw UserException( 8002 , "all servers down!" );
    }
    
    auto_ptr<DBClientCursor> SyncClusterConnection::getMore( const string &ns, long long cursorId, int nToReturn, int options ){
        uassert( 10022 , "SyncClusterConnection::getMore not supported yet" , 0); 
        auto_ptr<DBClientCursor> c;
        return c;
    }
    
    void SyncClusterConnection::insert( const string &ns, BSONObj obj ){ 
        string errmsg;
        if ( ! prepare( errmsg ) )
            throw UserException( 8003 , (string)"SyncClusterConnection::insert prepare failed: " + errmsg );

        for ( size_t i=0; i<_conns.size(); i++ ){
            _conns[i]->insert( ns , obj );
        }
        
        _checkLast();
    }
        
    void SyncClusterConnection::insert( const string &ns, const vector< BSONObj >& v ){ 
        uassert( 10023 , "SyncClusterConnection bulk insert not implemented" , 0); 
    }

    void SyncClusterConnection::remove( const string &ns , Query query, bool justOne ){ assert(0); }

    void SyncClusterConnection::update( const string &ns , Query query , BSONObj obj , bool upsert , bool multi ){ assert(0); }

    string SyncClusterConnection::_toString() const { 
        stringstream ss;
        ss << "SyncClusterConnection [";
        for ( size_t i=0; i<_conns.size(); i++ ){
            if ( i > 0 )
                ss << ",";
            ss << _conns[i]->toString();
        }
        ss << "]";
        return ss.str();
    }

    bool SyncClusterConnection::call( Message &toSend, Message &response, bool assertOk ){
        assert(0);
        return false;
    }
    
    void SyncClusterConnection::say( Message &toSend ){
        assert(0);
    }
    
    void SyncClusterConnection::sayPiggyBack( Message &toSend ){
        assert(0);
    }


}
