// stragegy.cpp

#include "stdafx.h"
#include "request.h"
#include "../client/connpool.h"
#include "../db/commands.h"

namespace mongo {

    // ----- Strategy ------

    void Strategy::doWrite( int op , Request& r , string server ){
        ScopedDbConnection dbcon( server );
        DBClientBase &_c = dbcon.conn();
        
        /* TODO FIX - do not case and call DBClientBase::say() */
        DBClientConnection&c = dynamic_cast<DBClientConnection&>(_c);
        c.port().say( r.m() );
        
        dbcon.done();
    }

    void Strategy::doQuery( Request& r , string server ){
        try{
            ScopedDbConnection dbcon( server );
            DBClientBase &_c = dbcon.conn();
            
            // TODO: This will not work with Paired connections.  Fix. 
            DBClientConnection&c = dynamic_cast<DBClientConnection&>(_c);
            Message response;
            bool ok = c.port().call( r.m(), response);
            uassert("mongos: error calling db", ok);
            r.reply( response );
            dbcon.done();
        }
        catch ( AssertionException& e ) {
            BSONObjBuilder err;
            err.append("$err", string("mongos: ") + (e.msg.empty() ? "assertion during query" : e.msg));
            BSONObj errObj = err.done();
            replyToQuery(QueryResult::ResultFlag_ErrSet, r.p() , r.m() , errObj);
        }
    }
    
    void Strategy::insert( string server , const char * ns , const BSONObj& obj ){
        ScopedDbConnection dbcon( server );
        dbcon->insert( ns , obj );
        dbcon.done();
    }

    // ----- ShardedCursor ------

    ShardedCursor::ShardedCursor( QueryMessage& q ){
        _ns = q.ns;
        _query = q.query.copy();
        _options = q.queryOptions;
        
        if ( q.fields.get() ){
            BSONObjBuilder b;
            for ( set<string>::iterator i=q.fields->begin(); i!=q.fields->end(); i++)
                b.append( i->c_str() , 1 );
            _fields = b.obj();
        }

        do {
            _id = security.getNonce();
        } while ( _id == 0 );
        
    }

    ShardedCursor::~ShardedCursor(){
    }
    
    auto_ptr<DBClientCursor> ShardedCursor::query( const string& server , int num ){
        ScopedDbConnection conn( server );
        log(5) << "ShardedCursor::query  server:" << server << " ns:" << _ns << " query:" << _query << " num:" << num << endl;
        auto_ptr<DBClientCursor> cursor = conn->query( _ns.c_str() , _query , num , 0 , ( _fields.isEmpty() ? 0 : &_fields ) , _options );
        conn.done();
        return cursor;
    }

    void ShardedCursor::sendNextBatch( Request& r ){
        BufBuilder b(32768);
        
        int num = 0;
        
        cout << "TEMP: ShardedCursor " << _ns << "\t" << _query << endl;
        while ( more() ){
            BSONObj o = next();
            cout << "\t" << o << endl;

            b.append( (void*)o.objdata() , o.objsize() );
            num++;
            
            if ( b.len() > 2097152 ){
                // TEMP
                break;
            }

        }

        uassert( "can't handle getMore with sharding yet" , ! more() );
        replyToQuery( 0 , r.p() , r.m() , b.buf() , b.len() , num );
    }
}
