// quorum.cpp

#include "stdafx.h"
#include "quorum.h"

namespace mongo {
    
    QuorumConnection::QuorumConnection( string commaSeperated ){
        string::size_type idx;
        while ( ( idx = commaSeperated.find( ',' ) ) != string::npos ){
            string h = commaSeperated.substr( 0 , idx );
            commaSeperated = commaSeperated.substr( idx + 1 );
            _connect( h );
        }
        _connect( commaSeperated );
        uassert( "QuorumConnection needs 3 servers" , _conns.size() == 3 );
    }

    QuorumConnection::QuorumConnection( string a , string b , string c ){
        // connect to all even if not working
        _connect( a );
        _connect( b );
        _connect( c );
    }

    QuorumConnection::~QuorumConnection(){
        for ( size_t i=0; i<_conns.size(); i++ )
            delete _conns[i];
        _conns.clear();
    }

    bool QuorumConnection::prepare( string& errmsg ){
        return fsync( errmsg );
    }
    
    bool QuorumConnection::fsync( string& errmsg ){
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

    void QuorumConnection::_checkLast(){
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
        throw UserException( (string)"QuorumConnection write op failed: " + err.str() );
    }

    void QuorumConnection::_connect( string host ){
        log() << "QuorumConnection connecting to: " << host << endl;
        DBClientConnection * c = new DBClientConnection( true );
        string errmsg;
        if ( ! c->connect( host , errmsg ) )
            log() << "QuorumConnection connect fail to: " << host << " errmsg: " << errmsg << endl;
        _conns.push_back( c );
    }

    auto_ptr<DBClientCursor> QuorumConnection::query(const string &ns, Query query, int nToReturn, int nToSkip,
                                                     const BSONObj *fieldsToReturn, int queryOptions){ 

        uassert( "$cmd not support yet in QuorumConnection::query" , ns.find( "$cmd" ) == string::npos );

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
        throw UserException( "all servers down!" );
    }
    
    auto_ptr<DBClientCursor> QuorumConnection::getMore( const string &ns, long long cursorId, int nToReturn, int options ){
        uassert("QuorumConnection::getMore not supported yet" , 0); 
        auto_ptr<DBClientCursor> c;
        return c;
    }
    
    void QuorumConnection::insert( const string &ns, BSONObj obj ){ 
        string errmsg;
        if ( ! prepare( errmsg ) )
            throw UserException( (string)"QuorumConnection::insert prepare failed: " + errmsg );

        for ( size_t i=0; i<_conns.size(); i++ ){
            _conns[i]->insert( ns , obj );
        }
        
        _checkLast();
    }
        
    void QuorumConnection::insert( const string &ns, const vector< BSONObj >& v ){ 
        uassert("QuorumConnection bulk insert not implemented" , 0); 
    }

    void QuorumConnection::remove( const string &ns , Query query, bool justOne ){ assert(0); }

    void QuorumConnection::update( const string &ns , Query query , BSONObj obj , bool upsert , bool multi ){ assert(0); }

    string QuorumConnection::toString(){ 
        stringstream ss;
        ss << "QuorumConnection [";
        for ( size_t i=0; i<_conns.size(); i++ ){
            if ( i > 0 )
                ss << ",";
            ss << _conns[i]->toString();
        }
        ss << "]";
        return ss.str();
    }


}
