// stragegy.cpp

#include "stdafx.h"
#include "request.h"
#include "../client/connpool.h"
#include "../db/commands.h"
#include "shard.h"

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
            
            checkShardVersion( _c , r.getns() );

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

    void checkShardVersion( DBClientBase& conn , const string& ns , bool authoritative ){
        // TODO: cache, optimize, etc...
        
        DBConfig * conf = grid.getDBConfig( ns );
        if ( ! conf )
            return;
        
        if ( ! conf->sharded( ns ) )
            return;
        
        
        ShardManager * manager = conf->getShardManager( ns , authoritative );
        
        ServerShardVersion version = manager->getVersion( conn.getServerAddress() );

        BSONObj result;
        if ( setShardVersion( conn , ns , version , authoritative , result ) ){
            return;
        }

        log(1) << "       setShardVersion failed!\n" << result << endl;

        if ( result.getBoolField( "need_authoritative" ) )
            massert( "need_authoritative set but in authoritative mode already" , ! authoritative );
        
        if ( ! authoritative ){
            checkShardVersion( conn , ns , 1 );
            return;
        }
        
        log(1) << "     setShardVersion failed: " << result << endl;
        massert( "setShardVersion failed!" , 0 );
    }
    
    bool setShardVersion( DBClientBase & conn , const string& ns , ServerShardVersion version , bool authoritative , BSONObj& result ){

        BSONObjBuilder cmdBuilder;
        cmdBuilder.append( "setShardVersion" , ns.c_str() );
        cmdBuilder.append( "configdb" , configServer.modelServer() );
        cmdBuilder.appendTimestamp( "version" , version );
        if ( authoritative )
            cmdBuilder.appendBool( "authoritative" , 1 );
        BSONObj cmd = cmdBuilder.obj();
        
        log(1) << "    setShardVersion  " << conn.getServerAddress() << "  " << ns << "  " << cmd << endl;
        
        return conn.runCommand( "admin" , cmd , result );
    }
    
}
