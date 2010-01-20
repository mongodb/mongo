// syncclusterconnection.cpp

#include "stdafx.h"
#include "syncclusterconnection.h"

// error codes 8000-8009

namespace mongo {
    
    SyncCluterConnection::SyncCluterConnection( string commaSeperated ){
        string::size_type idx;
        while ( ( idx = commaSeperated.find( ',' ) ) != string::npos ){
            string h = commaSeperated.substr( 0 , idx );
            commaSeperated = commaSeperated.substr( idx + 1 );
            _connect( h );
        }
        _connect( commaSeperated );
        uassert( 8004 ,  "SyncCluterConnection needs 3 servers" , _conns.size() == 3 );
    }

    SyncCluterConnection::SyncCluterConnection( string a , string b , string c ){
        // connect to all even if not working
        _connect( a );
        _connect( b );
        _connect( c );
    }

    SyncCluterConnection::~SyncCluterConnection(){
        for ( size_t i=0; i<_conns.size(); i++ )
            delete _conns[i];
        _conns.clear();
    }

    bool SyncCluterConnection::prepare( string& errmsg ){
        return fsync( errmsg );
    }
    
    bool SyncCluterConnection::fsync( string& errmsg ){
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

    void SyncCluterConnection::_checkLast(){
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
        throw UserException( 8001 , (string)"SyncCluterConnection write op failed: " + err.str() );
    }

    void SyncCluterConnection::_connect( string host ){
        log() << "SyncCluterConnection connecting to: " << host << endl;
        DBClientConnection * c = new DBClientConnection( true );
        string errmsg;
        if ( ! c->connect( host , errmsg ) )
            log() << "SyncCluterConnection connect fail to: " << host << " errmsg: " << errmsg << endl;
        _conns.push_back( c );
    }

    auto_ptr<DBClientCursor> SyncCluterConnection::query(const string &ns, Query query, int nToReturn, int nToSkip,
                                                     const BSONObj *fieldsToReturn, int queryOptions){ 

        uassert( 10021 ,  "$cmd not support yet in SyncCluterConnection::query" , ns.find( "$cmd" ) == string::npos );

        for ( size_t i=0; i<_conns.size(); i++ ){
            try {
                auto_ptr<DBClientCursor> cursor = 
                    _conns[i]->query( ns , query , nToReturn , nToSkip , fieldsToReturn , queryOptions );
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
    
    auto_ptr<DBClientCursor> SyncCluterConnection::getMore( const string &ns, long long cursorId, int nToReturn, int options ){
        uassert( 10022 , "SyncCluterConnection::getMore not supported yet" , 0); 
        auto_ptr<DBClientCursor> c;
        return c;
    }
    
    void SyncCluterConnection::insert( const string &ns, BSONObj obj ){ 
        string errmsg;
        if ( ! prepare( errmsg ) )
            throw UserException( 8003 , (string)"SyncCluterConnection::insert prepare failed: " + errmsg );

        for ( size_t i=0; i<_conns.size(); i++ ){
            _conns[i]->insert( ns , obj );
        }
        
        _checkLast();
    }
        
    void SyncCluterConnection::insert( const string &ns, const vector< BSONObj >& v ){ 
        uassert( 10023 , "SyncCluterConnection bulk insert not implemented" , 0); 
    }

    void SyncCluterConnection::remove( const string &ns , Query query, bool justOne ){ assert(0); }

    void SyncCluterConnection::update( const string &ns , Query query , BSONObj obj , bool upsert , bool multi ){ assert(0); }

    string SyncCluterConnection::toString(){ 
        stringstream ss;
        ss << "SyncCluterConnection [";
        for ( size_t i=0; i<_conns.size(); i++ ){
            if ( i > 0 )
                ss << ",";
            ss << _conns[i]->toString();
        }
        ss << "]";
        return ss.str();
    }


}
