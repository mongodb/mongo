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


    struct BenchRunConfig {
        BenchRunConfig() {
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
        double seconds;

        BSONObj ops;

        bool active; // true at starts, gets set to false when should stop
        AtomicUInt threadsReady;

        bool error;
    };

    static bool _hasSpecial( const BSONObj& obj ) {
        BSONObjIterator i( obj );
        while ( i.more() ) {
            BSONElement e = i.next();
            if ( e.fieldName()[0] == '#' )
                return true;
            
            if ( ! e.isABSONObj() )
                continue;
            
            if ( _hasSpecial( e.Obj() ) )
                return true;
        }
        return false;
    }
    
    static void _fixField( BSONObjBuilder& b , const BSONElement& e ) {
        assert( e.type() == Object );
        
        BSONObj sub = e.Obj();
        assert( sub.nFields() == 1 );
        
        BSONElement f = sub.firstElement();
        if ( str::equals( "#RAND_INT" , f.fieldName() ) ) {
            BSONObjIterator i( f.Obj() );
            int min = i.next().numberInt();
            int max = i.next().numberInt();
            
            int x = min + ( rand() % ( max - min ) );
            b.append( e.fieldName() , x );
        }
        else {
            uasserted( 14811 , str::stream() << "invalid bench dynamic piece: " << f.fieldName() );
        }
        
    } 
    
    static void fixQuery( BSONObjBuilder& b  , const BSONObj& obj ) {
        BSONObjIterator i( obj );
        while ( i.more() ) {
            BSONElement e = i.next();
            
            if ( e.type() != Object ) {
                b.append( e );
                continue;
            }

            BSONObj sub = e.Obj();
            if ( sub.firstElement().fieldName()[0] != '#' ) {
                b.append( e );
                continue;
            }
            
            _fixField( b , e );
        }
    }

    
    static BSONObj fixQuery( const BSONObj& obj ) {
        if ( ! _hasSpecial( obj ) ) 
            return obj;
        
        BSONObjBuilder b( obj.objsize() + 128 );
        fixQuery( b , obj );
        return b.obj();
    }

    static void benchThread( BenchRunConfig * config ) {
        ScopedDbConnection conn( config->host );
        config->threadsReady++;

        while ( config->active ) {
            BSONObjIterator i( config->ops );
            while ( i.more() ) {
                BSONElement e = i.next();
                string ns = e["ns"].String();
                string op = e["op"].String();

                if ( op == "findOne" ) {
                    conn->findOne( ns , fixQuery( e["query"].Obj() ) );
                }
                else if ( op == "update" ) {
                    conn->update( ns , fixQuery( e["query"].Obj() ) , e["update"].Obj() );
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
    BSONObj benchRun( const BSONObj& argsFake, void* data ) {
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
            config.seconds = args["seconds"].number();


        config.ops = args["ops"].Obj();

        // execute

        ScopedDbConnection conn( config.host );

        //    start threads
        vector<boost::thread*> all;
        for ( unsigned i=0; i<config.parallel; i++ )
            all.push_back( new boost::thread( boost::bind( benchThread , &config ) ) );

        //    give them time to init
        while ( config.threadsReady < config.parallel )
            sleepmillis( 1 );

        BSONObj before;
        conn->simpleCommand( "admin" , &before , "serverStatus" );

        sleepmillis( (int)(1000.0 * config.seconds) );

        BSONObj after;
        conn->simpleCommand( "admin" , &after , "serverStatus" );

        conn.done();

        config.active = false;

        for ( unsigned i=0; i<all.size(); i++ )
            all[i]->join();

        if ( config.error )
            return BSON( "err" << 1 );

        // compute actual ops/sec

        before = before["opcounters"].Obj().copy();
        after = after["opcounters"].Obj().copy();
        
        bool totals = args["totals"].trueValue();

        BSONObjBuilder buf;
        if ( ! totals )
            buf.append( "note" , "values per second" );

        {
            BSONObjIterator i( after );
            while ( i.more() ) {
                BSONElement e = i.next();
                double x = e.number();
                x = x - before[e.fieldName()].number();
                if ( ! totals )
                    x = x / config.seconds;
                buf.append( e.fieldName() , x );
            }
        }
        BSONObj zoo = buf.obj();
        return BSON( "" << zoo );
    }

    void installBenchmarkSystem( Scope& scope ) {
        scope.injectNative( "benchRun" , benchRun );
    }

}
