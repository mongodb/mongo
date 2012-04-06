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

#include "mongo/scripting/bench.h"

#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/md5.h"
#include "mongo/util/timer.h"
#include "mongo/util/version.h"


// ---------------------------------
// ---- benchmarking system --------
// ---------------------------------

// TODO:  Maybe extract as library to avoid code duplication?
namespace {
    inline pcrecpp::RE_Options flags2options(const char* flags) {
        pcrecpp::RE_Options options;
        options.set_utf8(true);
        while ( flags && *flags ) {
            if ( *flags == 'i' )
                options.set_caseless(true);
            else if ( *flags == 'm' )
                options.set_multiline(true);
            else if ( *flags == 'x' )
                options.set_extended(true);
            flags++;
        }
        return options;
    }
}

namespace mongo {

    BenchRunConfig::BenchRunConfig() : _mutex( "BenchRunConfig" ) {
        host = "localhost";
        db = "test";
        username = "";
        password = "";

        parallel = 1;
        seconds = 1;
        handleErrors = false;
        hideErrors = false;
        hideResults = true;

        active = true;
        threadsReady = 0;
        error = false;
        errCount = 0;
        throwGLE = false;
        breakOnTrap = true;
        loopCommands = true;
    }

    BenchRunStats::BenchRunStats() :
        findOneTotalTimeMicros( 0 ),
        updateTotalTimeMicros( 0 ),
        insertTotalTimeMicros( 0 ),
        deleteTotalTimeMicros( 0 ),
        queryTotalTimeMicros( 0 ),
        findOneTotalOps( 0 ),
        updateTotalOps( 0 ),
        insertTotalOps( 0 ),
        deleteTotalOps( 0 ),
        queryTotalOps( 0 ),
        _mutex( "BenchRunStats" )
    {}

    BSONObj benchStart( const BSONObj& , void* );
    BSONObj benchFinish( const BSONObj& , void* );

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
        verify( e.type() == Object );

        BSONObj sub = e.Obj();
        verify( sub.nFields() == 1 );

        BSONElement f = sub.firstElement();
        if ( str::equals( "#RAND_INT" , f.fieldName() ) ) {
            BSONObjIterator i( f.Obj() );
            int min = i.next().numberInt();
            int max = i.next().numberInt();

            int x = min + ( rand() % ( max - min ) );

            if ( i.more() )
                x *= i.next().numberInt();

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

            if ( ! e.isABSONObj() ) {
                b.append( e );
                continue;
            }

            BSONObj sub = e.Obj();
            if ( sub.firstElement().fieldName()[0] == '#' ) {
                _fixField( b , e );
            }
            else {
                BSONObjBuilder xx( e.type() == Object ? b.subobjStart( e.fieldName() ) : b.subarrayStart( e.fieldName() ) );
                fixQuery( xx , sub );
                xx.done();
            }

        }
    }

    static BSONObj fixQuery( const BSONObj& obj ) {

        if ( ! _hasSpecial( obj ) ) 
            return obj;

        BSONObjBuilder b( obj.objsize() + 128 );
        fixQuery( b , obj );
        return b.obj();
    }



    static void _benchThread( BenchRunConfig * config, BenchRunStats* stats, DBClientBase* conn ) {
        verify( conn );
        long long count = 0;
        mongo::Timer timer;

        while ( config->active || ! config->loopCommands ) {
            BSONObjIterator i( config->ops );
            while ( i.more() ) {

                // Break out if we should stop and we're not running all commands then stopping
                if( ! config->active && config->loopCommands ) break;

                BSONElement e = i.next();

                string ns = e["ns"].String();
                string op = e["op"].String();

                int delay = e["delay"].eoo() ? 0 : e["delay"].Int();

                BSONObj context = e["context"].eoo() ? BSONObj() : e["context"].Obj();

                auto_ptr<Scope> scope;
                ScriptingFunction scopeFunc = 0;
                BSONObj scopeObj;

                if (config->username != "") {
                    string errmsg;
                    if (!conn->auth(config->db, config->username, config->password, errmsg)) {
                        uasserted(15931, "Authenticating to connection for _benchThread failed: " + errmsg);
                    }
                }

                bool check = ! e["check"].eoo();
                if( check ){
                    if ( e["check"].type() == CodeWScope || e["check"].type() == Code || e["check"].type() == String ) {
                        scope = globalScriptEngine->getPooledScope( ns );
                        verify( scope.get() );

                        if ( e.type() == CodeWScope ) {
                            scopeFunc = scope->createFunction( e["check"].codeWScopeCode() );
                            scopeObj = BSONObj( e.codeWScopeScopeData() );
                        }
                        else {
                            scopeFunc = scope->createFunction( e["check"].valuestr() );
                        }

                        scope->init( &scopeObj );
                        verify( scopeFunc );
                    }
                    else {
                        warning() << "Invalid check type detected in benchRun op : " << e << endl;
                        check = false;
                    }
                }

                try {
                    if ( op == "findOne" ) {

                        BSONObj result;
                        unsigned long long startTime = timer.micros();
                        result = conn->findOne( ns , fixQuery( e["query"].Obj() ) );
                        {
                            scoped_lock lock( stats->_mutex);
                            stats->findOneTotalTimeMicros  += timer.micros() - startTime;
                            stats->findOneTotalOps++;
                        }


                        if( check ){
                            int err = scope->invoke( scopeFunc , 0 , &result,  1000 * 60 , false );
                            if( err ){
                                log() << "Error checking in benchRun thread [findOne]" << causedBy( scope->getError() ) << endl;

                                scoped_lock lock( config->_mutex );
                                config->errCount++;

                                return;
                            }
                        }

                        if( ! config->hideResults || e["showResult"].trueValue() ) log() << "Result from benchRun thread [findOne] : " << result << endl;

                    }
                    else if ( op == "command" ) {

                        BSONObj result;
                        // TODO
                        /* bool ok = */ conn->runCommand( ns , fixQuery( e["command"].Obj() ), result, e["options"].numberInt() );

                        if( check ){
                            int err = scope->invoke( scopeFunc , 0 , &result,  1000 * 60 , false );
                            if( err ){
                                log() << "Error checking in benchRun thread [command]" << causedBy( scope->getError() ) << endl;

                                scoped_lock lock( config->_mutex );
                                config->errCount++;

                                return;
                            }
                        }

                        if( ! config->hideResults || e["showResult"].trueValue() ) log() << "Result from benchRun thread [command] : " << result << endl;

                    }
                    else if( op == "find" || op == "query" ) {

                        int limit = e["limit"].eoo() ? 0 : e["limit"].numberInt();
                        int skip = e["skip"].eoo() ? 0 : e["skip"].Int();
                        int options = e["options"].eoo() ? 0 : e["options"].Int();
                        int batchSize = e["batchSize"].eoo() ? 0 : e["batchSize"].Int();
                        BSONObj filter = e["filter"].eoo() ? BSONObj() : e["filter"].Obj();
                        int expected = e["expected"].eoo() ? -1 : e["expected"].Int();

                        auto_ptr<DBClientCursor> cursor;
                        unsigned long long startTime = timer.micros();
                        cursor = conn->query( ns, fixQuery( e["query"].Obj() ), limit, skip, &filter, options, batchSize );
                        {
                            scoped_lock lock( stats->_mutex);
                            stats->queryTotalTimeMicros += timer.micros() - startTime;
                            stats->queryTotalOps++;
                        }

                        int count = cursor->itcount();

                        if ( expected >= 0 &&  count != expected ) {
                            cout << "bench query on: " << ns << " expected: " << expected << " got: " << cout << endl;
                            verify(false);
                        }

                        if( check ){
                            BSONObj thisValue = BSON( "count" << count << "context" << context );
                            int err = scope->invoke( scopeFunc , 0 , &thisValue, 1000 * 60 , false );
                            if( err ){
                                log() << "Error checking in benchRun thread [find]" << causedBy( scope->getError() ) << endl;

                                scoped_lock lock( config->_mutex );
                                config->errCount++;

                                return;
                            }
                        }

                        if( ! config->hideResults || e["showResult"].trueValue() ) log() << "Result from benchRun thread [query] : " << count << endl;

                    }
                    else if( op == "update" ) {

                        bool multi = e["multi"].trueValue();
                        bool upsert = e["upsert"].trueValue();
                        BSONObj query = e["query"].eoo() ? BSONObj() : e["query"].Obj();
                        BSONObj update = e["update"].Obj();
                        unsigned long long startTime = timer.micros();
                        conn->update( ns, fixQuery( query ), update, upsert , multi );

                        {
                            scoped_lock lock( stats->_mutex);
                            stats->updateTotalTimeMicros  += timer.micros() - startTime;
                            stats->updateTotalOps++;
                        }

                        bool safe = e["safe"].trueValue();
                        if( safe ){
                            BSONObj result = conn->getLastErrorDetailed();

                            if( check ){
                                int err = scope->invoke( scopeFunc , 0 , &result, 1000 * 60 , false );
                                if( err ){
                                    log() << "Error checking in benchRun thread [update]" << causedBy( scope->getError() ) << endl;

                                    scoped_lock lock( config->_mutex );
                                    config->errCount++;

                                    return;
                                }
                            }

                            if( ! config->hideResults || e["showResult"].trueValue() ) log() << "Result from benchRun thread [safe update] : " << result << endl;

                            if( ! result["err"].eoo() && result["err"].type() == String && ( config->throwGLE || e["throwGLE"].trueValue() ) )
                                throw DBException( (string)"From benchRun GLE" + causedBy( result["err"].String() ),
                                                   result["code"].eoo() ? 0 : result["code"].Int() );
                        }
                    }
                    else if( op == "insert" ) {
                        unsigned long long startTime = timer.micros();
                        conn->insert( ns, fixQuery( e["doc"].Obj() ) );
                        {
                            scoped_lock lock( stats->_mutex);
                            stats->insertTotalTimeMicros  += timer.micros() - startTime;
                            stats->insertTotalOps++;
                        }

                        bool safe = e["safe"].trueValue();
                        if( safe ){
                            BSONObj result = conn->getLastErrorDetailed();

                            if( check ){
                                int err = scope->invoke( scopeFunc , 0 , &result, 1000 * 60 , false );
                                if( err ){
                                    log() << "Error checking in benchRun thread [insert]" << causedBy( scope->getError() ) << endl;

                                    scoped_lock lock( config->_mutex );
                                    config->errCount++;

                                    return;
                                }
                            }

                            if( ! config->hideResults || e["showResult"].trueValue() ) log() << "Result from benchRun thread [safe insert] : " << result << endl;

                            if( ! result["err"].eoo() && result["err"].type() == String && ( config->throwGLE || e["throwGLE"].trueValue() ) )
                                throw DBException( (string)"From benchRun GLE" + causedBy( result["err"].String() ),
                                                   result["code"].eoo() ? 0 : result["code"].Int() );
                        }
                    }
                    else if( op == "delete" || op == "remove" ) {

                        bool multi = e["multi"].eoo() ? true : e["multi"].trueValue();
                        BSONObj query = e["query"].eoo() ? BSONObj() : e["query"].Obj();
                        unsigned long long startTime = timer.micros();
                        conn->remove( ns, fixQuery( query ), ! multi );
                        {
                            scoped_lock lock( stats->_mutex);
                            stats->deleteTotalTimeMicros  += timer.micros() - startTime;
                            stats->deleteTotalOps++;
                        }

                        bool safe = e["safe"].trueValue();
                        if( safe ){
                            BSONObj result = conn->getLastErrorDetailed();

                            if( check ){
                                int err = scope->invoke( scopeFunc , 0 , &result, 1000 * 60 , false );
                                if( err ){
                                    log() << "Error checking in benchRun thread [delete]" << causedBy( scope->getError() ) << endl;

                                    scoped_lock lock( config->_mutex );
                                    config->errCount++;

                                    return;
                                }
                            }

                            if( ! config->hideResults || e["showResult"].trueValue() ) log() << "Result from benchRun thread [safe remove] : " << result << endl;

                            if( ! result["err"].eoo() && result["err"].type() == String && ( config->throwGLE || e["throwGLE"].trueValue() ) )
                                throw DBException( (string)"From benchRun GLE " + causedBy( result["err"].String() ),
                                                   result["code"].eoo() ? 0 : result["code"].Int() );
                        }
                    }
                    else if ( op == "createIndex" ) {
                        conn->ensureIndex( ns , e["key"].Obj() , false , "" , false );
                    }
                    else if ( op == "dropIndex" ) {
                        conn->dropIndex( ns , e["key"].Obj()  );
                    }
                    else {
                        log() << "don't understand op: " << op << endl;
                        config->error = true;
                        return;
                    }
                }
                catch( DBException& ex ){
                    if( ! config->hideErrors || e["showError"].trueValue() ){

                        bool yesWatch = ( config->watchPattern && config->watchPattern->FullMatch( ex.what() ) );
                        bool noWatch = ( config->noWatchPattern && config->noWatchPattern->FullMatch( ex.what() ) );

                        if( ( ! config->watchPattern && config->noWatchPattern && ! noWatch ) || // If we're just ignoring things
                            ( ! config->noWatchPattern && config->watchPattern && yesWatch ) || // If we're just watching things
                            ( config->watchPattern && config->noWatchPattern && yesWatch && ! noWatch ) )
                            log() << "Error in benchRun thread for op " << e << causedBy( ex ) << endl;
                    }

                    bool yesTrap = ( config->trapPattern && config->trapPattern->FullMatch( ex.what() ) );
                    bool noTrap = ( config->noTrapPattern && config->noTrapPattern->FullMatch( ex.what() ) );

                    if( ( ! config->trapPattern && config->noTrapPattern && ! noTrap ) ||
                        ( ! config->noTrapPattern && config->trapPattern && yesTrap ) ||
                        ( config->trapPattern && config->noTrapPattern && yesTrap && ! noTrap ) ){
                        {
                            scoped_lock lock( config->_mutex );
                            config->trapped.append( BSON( "error" << ex.what() << "op" << e << "count" << count ) );
                        }
                        if( config->breakOnTrap ) return;
                    }
                    if( ! config->handleErrors && ! e["handleError"].trueValue() ) return;

                    {
                        scoped_lock lock( config->_mutex );
                        config->errCount++;
                    }
                }
                catch( ... ){
                    if( ! config->hideErrors || e["showError"].trueValue() ) log() << "Error in benchRun thread caused by unknown error for op " << e << endl;
                    if( ! config->handleErrors && ! e["handleError"].trueValue() ) return;

                    {
                        scoped_lock lock( config->_mutex );
                        config->errCount++;
                    }
                }
                
                if ( ++count % 100 == 0 ) {
                    conn->getLastError();
                }

                sleepmillis( delay );
            }

            if( ! config->loopCommands ) break;
        }
    }

    static void benchThread( BenchRunConfig * config, BenchRunStats * stats ) {

        ScopedDbConnection conn( config->host );
        config->threadsReady++;
        config->threadsActive++;

        try {
            if (config->username != "") {
                string errmsg;
                if (!conn.get()->auth(config->db, config->username, config->password, errmsg)) {
                    uasserted(15932, "Authenticating to connection for benchThread failed: " + errmsg);
                }
            }
            _benchThread( config, stats, conn.get() );
        }
        catch( DBException& e ){
            error() << "DBException not handled in benchRun thread" << causedBy( e ) << endl;
        }
        catch( std::exception& e ){
            error() << "Exception not handled in benchRun thread" << causedBy( e ) << endl;
        }
        catch( ... ){
            error() << "Exception not handled in benchRun thread." << endl;
        }
        conn->getLastError();
        config->threadsActive--;
        conn.done();

    }


    BenchRunner::BenchRunner( ) {}
    BenchRunner::~BenchRunner() {}

    void BenchRunner::init( BSONObj& args ) {

        oid.init();
        activeRuns[ oid ] = this;

        if ( args["host"].type() == String )
            config.host = args["host"].String();
        if ( args["db"].type() == String )
            config.db = args["db"].String();
        if ( args["username"].type() == String )
            config.username = args["username"].String();
        if ( args["password"].type() == String )
            config.db = args["password"].String();

        if ( args["parallel"].isNumber() )
            config.parallel = args["parallel"].numberInt();
        if ( args["seconds"].isNumber() )
            config.seconds = args["seconds"].number();
        if ( ! args["hideResults"].eoo() )
            config.hideResults = args["hideResults"].trueValue();
        if ( ! args["handleErrors"].eoo() )
            config.handleErrors = args["handleErrors"].trueValue();
        if ( ! args["hideErrors"].eoo() )
            config.hideErrors = args["hideErrors"].trueValue();
        if ( ! args["throwGLE"].eoo() )
            config.throwGLE = args["throwGLE"].trueValue();
        if ( ! args["breakOnTrap"].eoo() )
            config.breakOnTrap = args["breakOnTrap"].trueValue();
        if ( ! args["loopCommands"].eoo() )
            config.loopCommands = args["loopCommands"].trueValue();

        if ( ! args["trapPattern"].eoo() ){
            const char* regex = args["trapPattern"].regex();
            const char* flags = args["trapPattern"].regexFlags();
            config.trapPattern = shared_ptr< pcrecpp::RE >( new pcrecpp::RE( regex, flags2options( flags ) ) );
        }

        if ( ! args["noTrapPattern"].eoo() ){
            const char* regex = args["noTrapPattern"].regex();
            const char* flags = args["noTrapPattern"].regexFlags();
            config.noTrapPattern = shared_ptr< pcrecpp::RE >( new pcrecpp::RE( regex, flags2options( flags ) ) );
        }

        if ( ! args["watchPattern"].eoo() ){
            const char* regex = args["watchPattern"].regex();
            const char* flags = args["watchPattern"].regexFlags();
            config.watchPattern = shared_ptr< pcrecpp::RE >( new pcrecpp::RE( regex, flags2options( flags ) ) );
        }

        if ( ! args["noWatchPattern"].eoo() ){
            const char* regex = args["noWatchPattern"].regex();
            const char* flags = args["noWatchPattern"].regexFlags();
            config.noWatchPattern = shared_ptr< pcrecpp::RE >( new pcrecpp::RE( regex, flags2options( flags ) ) );
        }

        config.ops = args["ops"].Obj().getOwned();
        conn = shared_ptr< ScopedDbConnection >( new ScopedDbConnection( config.host ) );

        // Get initial stats
        conn->get()->simpleCommand( "admin" , &before , "serverStatus" );

        // Start threads
        for ( unsigned i = 0; i < config.parallel; i++ )
            threads.push_back( shared_ptr< boost::thread >( new boost::thread( boost::bind( benchThread , &config, &stats ) ) ) );

        // Give them time to init
        while ( config.threadsReady < config.parallel ) sleepmillis( 1 );

    }

    void BenchRunner::done() {

        log() << "Ending! (waiting for " << threads.size() << " threads)" << endl;

        {
            scoped_lock lock( config._mutex );
            config.active = false;
        }

        for ( unsigned i = 0; i < threads.size(); i++ ) threads[i]->join();

        // Get final stats
        conn->get()->simpleCommand( "admin" , &after , "serverStatus" );
        after.getOwned();

        conn.get()->done();

        activeRuns.erase( oid );

    }

    BSONObj BenchRunner::status() {
        scoped_lock lock( config._mutex );
        return BSON( "errCount" << config.errCount <<
                     "trappedCount" << config.trapped.arrSize() <<
                     "threadsActive" << config.threadsActive.get() );
    }

    BenchRunner* BenchRunner::get( BSONObj args ) {
        BenchRunner* runner = new BenchRunner();
        runner->init( args );
        return runner;
    }

    BenchRunner* BenchRunner::get( OID oid ) {
        return activeRuns[ oid ];
    }

    BSONObj BenchRunner::finish( BenchRunner* runner ) {

        runner->done();

        // vector<BSONOBj> errors = runner->config.errors;
        bool error = runner->config.error;

        if ( error )
            return BSON( "err" << 1 );

        // compute actual ops/sec
        BSONObj before = runner->before["opcounters"].Obj();
        BSONObj after = runner->after["opcounters"].Obj();

        BSONObjBuilder buf;
        buf.append( "note" , "values per second" );
        buf.append( "errCount", (long long) runner->config.errCount );
        buf.append( "trapped", runner->config.trapped.arr() );
        if( runner->stats.findOneTotalOps )
            buf.append( "findOneLatencyAverageMs",
                        static_cast<double> ( ( runner->stats.findOneTotalTimeMicros/1000 ) / runner->stats.findOneTotalOps ) );
        if( runner->stats.insertTotalOps )
            buf.append( "insertLatencyAverageMs",
                        static_cast<double> ( ( runner->stats.insertTotalTimeMicros/1000 ) / runner->stats.insertTotalOps ) );
        if( runner->stats.deleteTotalOps )
            buf.append( "deleteLatencyAverageMs",
                        static_cast<double> ( ( runner->stats.deleteTotalTimeMicros/1000 ) / runner->stats.deleteTotalOps ) );
        if( runner->stats.updateTotalOps )
            buf.append( "updateLatencyAverageMs",
                        static_cast<double> ( ( runner->stats.updateTotalTimeMicros/1000 ) / runner->stats.updateTotalOps ) );
        if( runner->stats.queryTotalOps )
            buf.append( "queryLatencyAverageMs",
                        static_cast<double> ( ( runner->stats.queryTotalTimeMicros/1000 ) / runner->stats.queryTotalOps ) );
        {
            BSONObjIterator i( after );
            while ( i.more() ) {
                BSONElement e = i.next();
                double x = e.number();
                x = x - before[e.fieldName()].number();
                buf.append( e.fieldName() , x / runner->config.seconds );
            }
        }

        BSONObj zoo = buf.obj();

        delete runner;
        return zoo;
    }

    map< OID, BenchRunner* > BenchRunner::activeRuns;

    /**
     * benchRun( { ops : [] , host : XXX , db : XXXX , parallel : 5 , seconds : 5 }
     */
    BSONObj benchRunSync( const BSONObj& argsFake, void* data ) {

        BSONObj start = benchStart( argsFake, data );

        OID oid = OID( start.firstElement().String() );
        BenchRunner* runner = BenchRunner::get( oid );
        sleepmillis( (int)(1000.0 * runner->config.seconds) );

        return benchFinish( start, data );
    }

    /**
     * benchRun( { ops : [] , host : XXX , db : XXXX , parallel : 5 , seconds : 5 }
     */
    BSONObj benchStart( const BSONObj& argsFake, void* data ) {

        verify( argsFake.firstElement().isABSONObj() );
        BSONObj args = argsFake.firstElement().Obj();

        // Get new BenchRunner object
        BenchRunner* runner = BenchRunner::get( args );

        log() << "Starting benchRun test " << runner->oid << endl;

        return BSON( "" << runner->oid.toString() );
    }

    /**
     * benchRun( { ops : [] , host : XXX , db : XXXX , parallel : 5 , seconds : 5 }
     */
    BSONObj benchStatus( const BSONObj& argsFake, void* data ) {

        OID oid = OID( argsFake.firstElement().String() );

        log() << "Getting status for benchRun test " << oid << endl;

        // Get BenchRunner object
        BenchRunner* runner = BenchRunner::get( oid );

        BSONObj statusObj = runner->status();

        return BSON( "" << statusObj );
    }

    /**
     * benchRun( { ops : [] , host : XXX , db : XXXX , parallel : 5 , seconds : 5 }
     */
    BSONObj benchFinish( const BSONObj& argsFake, void* data ) {

        OID oid = OID( argsFake.firstElement().String() );

        log() << "Finishing benchRun test " << oid << endl;

        // Get new BenchRunner object
        BenchRunner* runner = BenchRunner::get( oid );

        BSONObj finalObj = BenchRunner::finish( runner );

        return BSON( "" << finalObj );
    }

    void installBenchmarkSystem( Scope& scope ) {
        scope.injectNative( "benchRun" , benchRunSync );
        scope.injectNative( "benchRunSync" , benchRunSync );
        scope.injectNative( "benchStart" , benchStart );
        scope.injectNative( "benchStatus" , benchStatus );
        scope.injectNative( "benchFinish" , benchFinish );
    }

}
