// stragegy.cpp

#include "stdafx.h"
#include "request.h"
#include "../client/connpool.h"
#include "../db/commands.h"
#include "../db/queryutil.h"

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
        _skip = q.ntoskip;
        _ntoreturn = q.ntoreturn;
        
        _totalSent = 0;

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
    
    auto_ptr<DBClientCursor> ShardedCursor::query( const string& server , int num , BSONObj extra ){

        BSONObj q = _query;
        if ( ! extra.isEmpty() ){
            q = concatQuery( q , extra );
        }

        ScopedDbConnection conn( server );
        log(5) << "ShardedCursor::query  server:" << server << " ns:" << _ns << " query:" << q << " num:" << num << endl;
        auto_ptr<DBClientCursor> cursor = conn->query( _ns.c_str() , _query , num , 0 , ( _fields.isEmpty() ? 0 : &_fields ) , _options );
        conn.done();
        return cursor;
    }

    BSONObj ShardedCursor::concatQuery( const BSONObj& query , const BSONObj& extraFilter ){
        if ( ! query.hasField( "query" ) )
            return _concatFilter( query , extraFilter );
        
        BSONObjBuilder b;
        BSONObjIterator i( query );
        while ( i.more() ){
            BSONElement e = i.next();
            if ( e.eoo() )
                break;

            if ( strcmp( e.fieldName() , "query" ) ){
                b.append( e );
                continue;
            }
            
            b.append( "query" , _concatFilter( e.embeddedObjectUserCheck() , extraFilter ) );
        }
        return b.obj();
    }
    
    BSONObj ShardedCursor::_concatFilter( const BSONObj& filter , const BSONObj& extra ){
        BSONObjBuilder b;
        b.appendElements( filter );
        b.appendElements( extra );
        
        FieldBoundSet s( "wrong" , b.obj() );
        return s.simplifiedQuery();
    }

    bool ShardedCursor::sendNextBatch( Request& r ){
        int maxSize = 1024 * 1024;
        if ( _totalSent > 0 )
            maxSize *= 3;
        
        BufBuilder b(32768);
        
        int num = 0;
        bool sendMore = true;
        
        cout << "TEMP: ShardedCursor " << _ns << "\t" << _query << " ntoreturn: " << _ntoreturn << endl;
        while ( more() ){
            BSONObj o = next();
            cout << "\t" << o << endl;

            b.append( (void*)o.objdata() , o.objsize() );
            num++;
            
            if ( b.len() > maxSize )
                break;

            if ( num == _ntoreturn ){
                // soft limit aka batch size
                break;
            }

            if ( ( -1 * num + _totalSent ) == _ntoreturn ){
                // hard limit - total to send
                sendMore = false;
                break;
            }
        }

        bool hasMore = sendMore && more();
        log() << "  hasMore:" << hasMore << " id:" << _id << endl;
        replyToQuery( 0 , r.p() , r.m() , b.buf() , b.len() , num , 0 , hasMore ? _id : 0 );
        _totalSent += num;
        return hasMore;
    }
}
