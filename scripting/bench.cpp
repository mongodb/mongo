/** @file bench.cpp */

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
 */

#include "pch.h"
#include "engine.h"
#include "../util/md5.hpp"
#include "../util/version.h"
#include "../client/dbclient.h"
#include "../client/connpool.h"
// ---------------------------------
// ---- benchmarking system --------
// ---------------------------------


namespace mongo {


    /**
     * benchQuery( "foo" , { _id : 1 } )
     */
    BSONObj benchQuery( const BSONObj& args ){
        return BSONObj();
    }

    struct BenchRunConfig {
        BenchRunConfig(){
            host = "localhost";
            db = "test";
            
            parallel = 1;
            seconds = 1;

            active = true;
            threadsReady = 0;
            error = false;
        }
        
        string host;
        string db;
        
        unsigned parallel;
        int seconds;

        BSONObj ops;

        bool active; // true at starts, gets set to false when should stop
        AtomicUInt threadsReady;

        bool error;
    };
    
    void runBench( BenchRunConfig * config ){
        ScopedDbConnection conn( config->host );
        config->threadsReady++;        
        
        while ( config->active ){
            BSONObjIterator i( config->ops );
            while ( i.more() ){
                BSONElement e = i.next();
                string ns = e["ns"].String();
                string op = e["op"].String();
                
                if ( op == "findOne" ) {
                    conn->findOne( ns , e["query"].Obj() );
                }
                else {
                    log() << "don't understand op: " << op << endl;
                    config->error = true;
                    return;
                }

            }
        }

        conn.done();
    }
    
    /**
     * benchRun( { ops : [] , host : XXX , db : XXXX , parallel : 5 , seconds : 5 }
     */
    BSONObj benchRun( const BSONObj& argsFake ){
        assert( argsFake.firstElement().isABSONObj() );
        BSONObj args = argsFake.firstElement().Obj();

        // setup

        BenchRunConfig config;
        
        if ( args["host"].type() == String )
            config.host = args["host"].String();
        if ( args["db"].type() == String )
            config.db = args["db"].String();

        if ( args["parallel"].isNumber() )
            config.parallel = args["parallel"].numberInt();
        if ( args["seconds"].isNumber() )
            config.seconds = args["seconds"].numberInt();


        config.ops = args["ops"].Obj();

        // execute

        ScopedDbConnection conn( config.host );

        //    start threads
        vector<boost::thread*> all;
        for ( unsigned i=0; i<config.parallel; i++ )
            all.push_back( new boost::thread( boost::bind( runBench , &config ) ) );
        
        //    give them time to init
        while ( config.threadsReady < config.parallel )
            sleepmillis( 1 );

        BSONObj before;
        conn->simpleCommand( "admin" , &before , "serverStatus" );
        
        sleepsecs( config.seconds );

        BSONObj after;
        conn->simpleCommand( "admin" , &after , "serverStatus" );
        
        conn.done();

        config.active = false;
        
        for ( unsigned i=0; i<all.size(); i++ )
            all[i]->join();
        
        if ( config.error )
            return BSON( "err" << 1 );
        
        // compute actual ops/sec
        
        before = before["opcounters"].Obj();
        after = after["opcounters"].Obj();
        
        BSONObjBuilder buf;
        buf.append( "note" , "values per second" );

        {
            BSONObjIterator i( after );
            while ( i.more() ){
                BSONElement e = i.next();
                double x = e.number();
                x = x - before[e.fieldName()].number();
                buf.append( e.fieldName() , x / config.seconds );
            }
        }
        BSONObj zoo = buf.obj();
        return BSON( "" << zoo );
    }

    void installBenchmarkSystem( Scope& scope ){
        scope.injectNative( "benchRun" , benchRun );
    }

}
