// quorum.cpp

#include "../stdafx.h"
#include "quorum.h"

namespace mongo {
    
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
            if ( _conns[i]->simpleCommand( "admin" , 0 , "fsync" ) )
                continue;
            ok = false;
            errmsg += _conns[i]->toString() + ":" + res.toString();
        }
        return ok;
    }

    void QuorumConnection::_connect( string host ){
        DBClientConnection * c = new DBClientConnection( true );
        string errmsg;
        if ( ! c->connect( host , errmsg ) )
            log() << "QuorumConnection connect fail to: " << host << " errmsg: " << errmsg << endl;
        _conns.push_back( c );
    }
};
