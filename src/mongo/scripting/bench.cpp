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

    BenchRunEventCounter::BenchRunEventCounter() {
        reset();
    }

    BenchRunEventCounter::~BenchRunEventCounter() {}

    void BenchRunEventCounter::reset() {
        _numEvents = 0;
        _totalTimeMicros = 0;
    }

    void BenchRunEventCounter::updateFrom(const BenchRunEventCounter &other) {
        _numEvents += other._numEvents;
        _totalTimeMicros += other._totalTimeMicros;
    }

    BenchRunStats::BenchRunStats() {
        reset();
    }

    BenchRunStats::~BenchRunStats() {}

    void BenchRunStats::reset() {
        error = false;
        errCount = 0;

        findOneCounter.reset();
        updateCounter.reset();
        insertCounter.reset();
        deleteCounter.reset();
        queryCounter.reset();

        trappedErrors.clear();
    }

    void BenchRunStats::updateFrom(const BenchRunStats &other) {
        if (other.error)
            error = true;
        errCount += other.errCount;

        findOneCounter.updateFrom(other.findOneCounter);
        updateCounter.updateFrom(other.updateCounter);
        insertCounter.updateFrom(other.insertCounter);
        deleteCounter.updateFrom(other.deleteCounter);
        queryCounter.updateFrom(other.queryCounter);

        for (size_t i = 0; i < other.trappedErrors.size(); ++i)
            trappedErrors.push_back(other.trappedErrors[i]);
    }

    BenchRunConfig::BenchRunConfig() {
        initializeToDefaults();
    }

    void BenchRunConfig::initializeToDefaults() {
        host = "localhost";
        db = "test";
        username = "";
        password = "";

        parallel = 1;
        seconds = 1;
        hideResults = true;
        handleErrors = false;
        hideErrors = false;

        trapPattern.reset();
        noTrapPattern.reset();
        watchPattern.reset();
        noWatchPattern.reset();

        ops = BSONObj();

        throwGLE = false;
        breakOnTrap = true;
    }

    BenchRunConfig *BenchRunConfig::createFromBson( const BSONObj &args ) {
        BenchRunConfig *config = new BenchRunConfig();
        config->initializeFromBson( args );
        return config;
    }

    void BenchRunConfig::initializeFromBson( const BSONObj &args ) {
        initializeToDefaults();

        if ( args["host"].type() == String )
            this->host = args["host"].String();
        if ( args["db"].type() == String )
            this->db = args["db"].String();
        if ( args["username"].type() == String )
            this->username = args["username"].String();
        if ( args["password"].type() == String )
            this->db = args["password"].String();

        if ( args["parallel"].isNumber() )
            this->parallel = args["parallel"].numberInt();
        if ( args["seconds"].isNumber() )
            this->seconds = args["seconds"].number();
        if ( ! args["hideResults"].eoo() )
            this->hideResults = args["hideResults"].trueValue();
        if ( ! args["handleErrors"].eoo() )
            this->handleErrors = args["handleErrors"].trueValue();
        if ( ! args["hideErrors"].eoo() )
            this->hideErrors = args["hideErrors"].trueValue();
        if ( ! args["throwGLE"].eoo() )
            this->throwGLE = args["throwGLE"].trueValue();
        if ( ! args["breakOnTrap"].eoo() )
            this->breakOnTrap = args["breakOnTrap"].trueValue();

        uassert(16164, "loopCommands config not supported", args["loopCommands"].eoo());

        if ( ! args["trapPattern"].eoo() ){
            const char* regex = args["trapPattern"].regex();
            const char* flags = args["trapPattern"].regexFlags();
            this->trapPattern = shared_ptr< pcrecpp::RE >( new pcrecpp::RE( regex, flags2options( flags ) ) );
        }

        if ( ! args["noTrapPattern"].eoo() ){
            const char* regex = args["noTrapPattern"].regex();
            const char* flags = args["noTrapPattern"].regexFlags();
            this->noTrapPattern = shared_ptr< pcrecpp::RE >( new pcrecpp::RE( regex, flags2options( flags ) ) );
        }

        if ( ! args["watchPattern"].eoo() ){
            const char* regex = args["watchPattern"].regex();
            const char* flags = args["watchPattern"].regexFlags();
            this->watchPattern = shared_ptr< pcrecpp::RE >( new pcrecpp::RE( regex, flags2options( flags ) ) );
        }

        if ( ! args["noWatchPattern"].eoo() ){
            const char* regex = args["noWatchPattern"].regex();
            const char* flags = args["noWatchPattern"].regexFlags();
            this->noWatchPattern = shared_ptr< pcrecpp::RE >( new pcrecpp::RE( regex, flags2options( flags ) ) );
        }

        this->ops = args["ops"].Obj().getOwned();
    }

    DBClientBase *BenchRunConfig::createConnection() const {
        std::string errorMessage;
        ConnectionString connectionString = ConnectionString::parse( host, errorMessage  );
        uassert( 16157, errorMessage, connectionString.isValid() );
        DBClientBase *connection = connectionString.connect(errorMessage);
        uassert( 16158, errorMessage, connection != NULL );
        return connection;
    }

    BenchRunState::BenchRunState( unsigned numWorkers )
        : _mutex(),
          _numUnstartedWorkers( numWorkers ),
          _numActiveWorkers( 0 ),
          _isShuttingDown( 0 ) {
    }

    BenchRunState::~BenchRunState() {
        wassert(_numActiveWorkers == 0 && _numUnstartedWorkers == 0);
    }

    void BenchRunState::waitForState(State awaitedState) {
        boost::mutex::scoped_lock lk(_mutex);

        switch ( awaitedState ) {
        case BRS_RUNNING:
            while ( _numUnstartedWorkers > 0 ) {
                massert( 16147, "Already finished.", _numUnstartedWorkers + _numActiveWorkers > 0 );
                _stateChangeCondition.wait( _mutex );
            }
            break;
        case BRS_FINISHED:
            while ( _numUnstartedWorkers + _numActiveWorkers > 0 ) {
                _stateChangeCondition.wait( _mutex );
            }
            break;
        default:
            msgasserted(16152, mongoutils::str::stream() << "Cannot wait for state " << awaitedState);
        }
    }

    void BenchRunState::tellWorkersToFinish() {
        _isShuttingDown.set( 1 );
    }

    void BenchRunState::assertFinished() {
        boost::mutex::scoped_lock lk(_mutex);
        verify(0 == _numUnstartedWorkers + _numActiveWorkers);
    }

    bool BenchRunState::shouldWorkerFinish() {
        return bool(_isShuttingDown.get());
    }

    void BenchRunState::onWorkerStarted() {
        boost::mutex::scoped_lock lk(_mutex);
        verify( _numUnstartedWorkers > 0 );
        --_numUnstartedWorkers;
        ++_numActiveWorkers;
        if (_numUnstartedWorkers == 0) {
            _stateChangeCondition.notify_all();
        }
    }

    void BenchRunState::onWorkerFinished() {
        boost::mutex::scoped_lock lk(_mutex);
        verify( _numActiveWorkers > 0 );
        --_numActiveWorkers;
        if (_numActiveWorkers + _numUnstartedWorkers == 0) {
            _stateChangeCondition.notify_all();
        }
    }

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

    BenchRunWorker::BenchRunWorker(const BenchRunConfig *config, BenchRunState *brState)
        : _config(config), _brState(brState) {
    }

    BenchRunWorker::~BenchRunWorker() {}

    void BenchRunWorker::start() {
        boost::thread(boost::bind(&BenchRunWorker::run, this));
    }

    bool BenchRunWorker::shouldStop() const {
        return _brState->shouldWorkerFinish();
    }

    void BenchRunWorker::generateLoadOnConnection( DBClientBase* conn ) {
        verify( conn );
        long long count = 0;
        mongo::Timer timer;

        while ( !shouldStop() ) {
            BSONObjIterator i( _config->ops );
            while ( i.more() ) {

                if ( shouldStop() ) break;

                BSONElement e = i.next();

                string ns = e["ns"].String();
                string op = e["op"].String();

                int delay = e["delay"].eoo() ? 0 : e["delay"].Int();

                BSONObj context = e["context"].eoo() ? BSONObj() : e["context"].Obj();

                auto_ptr<Scope> scope;
                ScriptingFunction scopeFunc = 0;
                BSONObj scopeObj;

                if (_config->username != "") {
                    string errmsg;
                    if (!conn->auth(_config->db, _config->username, _config->password, errmsg)) {
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
                        {
                            BenchRunEventTrace _bret(&_stats.findOneCounter);
                            result = conn->findOne( ns , fixQuery( e["query"].Obj() ) );
                        }

                        if( check ){
                            int err = scope->invoke( scopeFunc , 0 , &result,  1000 * 60 , false );
                            if( err ){
                                log() << "Error checking in benchRun thread [findOne]" << causedBy( scope->getError() ) << endl;

                                _stats.errCount++;

                                return;
                            }
                        }

                        if( ! _config->hideResults || e["showResult"].trueValue() ) log() << "Result from benchRun thread [findOne] : " << result << endl;

                    }
                    else if ( op == "command" ) {

                        BSONObj result;
                        // TODO
                        /* bool ok = */ conn->runCommand( ns , fixQuery( e["command"].Obj() ), result, e["options"].numberInt() );

                        if( check ){
                            int err = scope->invoke( scopeFunc , 0 , &result,  1000 * 60 , false );
                            if( err ){
                                log() << "Error checking in benchRun thread [command]" << causedBy( scope->getError() ) << endl;

                                _stats.errCount++;

                                return;
                            }
                        }

                        if( ! _config->hideResults || e["showResult"].trueValue() ) log() << "Result from benchRun thread [command] : " << result << endl;

                    }
                    else if( op == "find" || op == "query" ) {

                        int limit = e["limit"].eoo() ? 0 : e["limit"].numberInt();
                        int skip = e["skip"].eoo() ? 0 : e["skip"].Int();
                        int options = e["options"].eoo() ? 0 : e["options"].Int();
                        int batchSize = e["batchSize"].eoo() ? 0 : e["batchSize"].Int();
                        BSONObj filter = e["filter"].eoo() ? BSONObj() : e["filter"].Obj();
                        int expected = e["expected"].eoo() ? -1 : e["expected"].Int();

                        auto_ptr<DBClientCursor> cursor;

                        {
                            BenchRunEventTrace _bret(&_stats.queryCounter);
                            cursor = conn->query( ns, fixQuery( e["query"].Obj() ), limit, skip, &filter, options, batchSize );
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

                                _stats.errCount++;

                                return;
                            }
                        }

                        if( ! _config->hideResults || e["showResult"].trueValue() ) log() << "Result from benchRun thread [query] : " << count << endl;

                    }
                    else if( op == "update" ) {

                        bool multi = e["multi"].trueValue();
                        bool upsert = e["upsert"].trueValue();
                        BSONObj query = e["query"].eoo() ? BSONObj() : e["query"].Obj();
                        BSONObj update = e["update"].Obj();
                        BSONObj result;
                        bool safe = e["safe"].trueValue();

                        {
                            BenchRunEventTrace _bret(&_stats.updateCounter);
                            conn->update( ns, fixQuery( query ), update, upsert , multi );
                            if (safe)
                                result = conn->getLastErrorDetailed();
                        }

                        if( safe ){
                            if( check ){
                                int err = scope->invoke( scopeFunc , 0 , &result, 1000 * 60 , false );
                                if( err ){
                                    log() << "Error checking in benchRun thread [update]" << causedBy( scope->getError() ) << endl;

                                    _stats.errCount++;

                                    return;
                                }
                            }

                            if( ! _config->hideResults || e["showResult"].trueValue() ) log() << "Result from benchRun thread [safe update] : " << result << endl;

                            if( ! result["err"].eoo() && result["err"].type() == String && ( _config->throwGLE || e["throwGLE"].trueValue() ) )
                                throw DBException( (string)"From benchRun GLE" + causedBy( result["err"].String() ),
                                                   result["code"].eoo() ? 0 : result["code"].Int() );
                        }
                    }
                    else if( op == "insert" ) {
                        bool safe = e["safe"].trueValue();
                        BSONObj result;
                        {
                            BenchRunEventTrace _bret(&_stats.insertCounter);
                            conn->insert( ns, fixQuery( e["doc"].Obj() ) );
                            if (safe)
                                result = conn->getLastErrorDetailed();
                        }

                        if( safe ){
                            if( check ){
                                int err = scope->invoke( scopeFunc , 0 , &result, 1000 * 60 , false );
                                if( err ){
                                    log() << "Error checking in benchRun thread [insert]" << causedBy( scope->getError() ) << endl;

                                    _stats.errCount++;

                                    return;
                                }
                            }

                            if( ! _config->hideResults || e["showResult"].trueValue() ) log() << "Result from benchRun thread [safe insert] : " << result << endl;

                            if( ! result["err"].eoo() && result["err"].type() == String && ( _config->throwGLE || e["throwGLE"].trueValue() ) )
                                throw DBException( (string)"From benchRun GLE" + causedBy( result["err"].String() ),
                                                   result["code"].eoo() ? 0 : result["code"].Int() );
                        }
                    }
                    else if( op == "delete" || op == "remove" ) {

                        bool multi = e["multi"].eoo() ? true : e["multi"].trueValue();
                        BSONObj query = e["query"].eoo() ? BSONObj() : e["query"].Obj();
                        bool safe = e["safe"].trueValue();
                        BSONObj result;

                        {
                            BenchRunEventTrace _bret(&_stats.deleteCounter);
                            conn->remove( ns, fixQuery( query ), ! multi );
                            if (safe)
                                result = conn->getLastErrorDetailed();
                        }

                        if( safe ){
                            if( check ){
                                int err = scope->invoke( scopeFunc , 0 , &result, 1000 * 60 , false );
                                if( err ){
                                    log() << "Error checking in benchRun thread [delete]" << causedBy( scope->getError() ) << endl;

                                    _stats.errCount++;

                                    return;
                                }
                            }

                            if( ! _config->hideResults || e["showResult"].trueValue() ) log() << "Result from benchRun thread [safe remove] : " << result << endl;

                            if( ! result["err"].eoo() && result["err"].type() == String && ( _config->throwGLE || e["throwGLE"].trueValue() ) )
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
                        _stats.error = true;
                        return;
                    }
                }
                catch( DBException& ex ){
                    if( ! _config->hideErrors || e["showError"].trueValue() ){

                        bool yesWatch = ( _config->watchPattern && _config->watchPattern->FullMatch( ex.what() ) );
                        bool noWatch = ( _config->noWatchPattern && _config->noWatchPattern->FullMatch( ex.what() ) );

                        if( ( ! _config->watchPattern && _config->noWatchPattern && ! noWatch ) || // If we're just ignoring things
                            ( ! _config->noWatchPattern && _config->watchPattern && yesWatch ) || // If we're just watching things
                            ( _config->watchPattern && _config->noWatchPattern && yesWatch && ! noWatch ) )
                            log() << "Error in benchRun thread for op " << e << causedBy( ex ) << endl;
                    }

                    bool yesTrap = ( _config->trapPattern && _config->trapPattern->FullMatch( ex.what() ) );
                    bool noTrap = ( _config->noTrapPattern && _config->noTrapPattern->FullMatch( ex.what() ) );

                    if( ( ! _config->trapPattern && _config->noTrapPattern && ! noTrap ) ||
                        ( ! _config->noTrapPattern && _config->trapPattern && yesTrap ) ||
                        ( _config->trapPattern && _config->noTrapPattern && yesTrap && ! noTrap ) ){
                        {
                            _stats.trappedErrors.push_back( BSON( "error" << ex.what() << "op" << e << "count" << count ) );
                        }
                        if( _config->breakOnTrap ) return;
                    }
                    if( ! _config->handleErrors && ! e["handleError"].trueValue() ) return;

                    _stats.errCount++;
                }
                catch( ... ){
                    if( ! _config->hideErrors || e["showError"].trueValue() ) log() << "Error in benchRun thread caused by unknown error for op " << e << endl;
                    if( ! _config->handleErrors && ! e["handleError"].trueValue() ) return;

                    _stats.errCount++;
                }

                if ( ++count % 100 == 0 ) {
                    conn->getLastError();
                }

                sleepmillis( delay );
            }
        }
    }

    namespace {
        class BenchRunWorkerStateGuard : private boost::noncopyable {
        public:
            explicit BenchRunWorkerStateGuard( BenchRunState *brState ) : _brState( brState ) {
                _brState->onWorkerStarted();
            }

            ~BenchRunWorkerStateGuard() {
                _brState->onWorkerFinished();
            }

        private:
            BenchRunState *_brState;
        };
    }  // namespace

    void BenchRunWorker::run() {
        BenchRunWorkerStateGuard _workerStateGuard( _brState );

        boost::scoped_ptr<DBClientBase> conn( _config->createConnection() );

        try {
            if ( !_config->username.empty() ) {
                string errmsg;
                if (!conn->auth(_config->db, _config->username, _config->password, errmsg)) {
                    uasserted(15932, "Authenticating to connection for benchThread failed: " + errmsg);
                }
            }
            generateLoadOnConnection( conn.get() );
        }
        catch( DBException& e ){
            error() << "DBException not handled in benchRun thread" << causedBy( e ) << endl;
        }
        catch( std::exception& e ){
            error() << "std::exception not handled in benchRun thread" << causedBy( e ) << endl;
        }
        catch( ... ){
            error() << "Unknown exception not handled in benchRun thread." << endl;
        }
    }

    BenchRunner::BenchRunner( BenchRunConfig *config )
        : _brState(config->parallel),
          _config(config) {

        _oid.init();
        boost::mutex::scoped_lock lk(_staticMutex);
         _activeRuns[_oid] = this;
     }

     BenchRunner::~BenchRunner() {
         for (size_t i = 0; i < _workers.size(); ++i)
             delete _workers[i];
     }

     void BenchRunner::start( ) {


         {
             boost::scoped_ptr<DBClientBase> conn( _config->createConnection() );
             // Get initial stats
             conn->simpleCommand( "admin" , &before , "serverStatus" );
             before = before.getOwned();
         }

         // Start threads
         for ( unsigned i = 0; i < _config->parallel; i++ ) {
             BenchRunWorker *worker = new BenchRunWorker(_config.get(), &_brState);
             worker->start();
             _workers.push_back(worker);
         }

         _brState.waitForState(BenchRunState::BRS_RUNNING);
     }

     void BenchRunner::stop() {
         _brState.tellWorkersToFinish();
         _brState.waitForState(BenchRunState::BRS_FINISHED);

         {
             boost::scoped_ptr<DBClientBase> conn( _config->createConnection() );
             // Get final stats
             conn->simpleCommand( "admin" , &after , "serverStatus" );
             after = after.getOwned();
         }

         {
             boost::mutex::scoped_lock lk(_staticMutex);
             _activeRuns.erase( _oid );
         }
     }

     BenchRunner* BenchRunner::createWithConfig( const BSONObj &configArgs ) {
         BenchRunConfig *config = BenchRunConfig::createFromBson( configArgs );
         return new BenchRunner(config);
     }

     BenchRunner* BenchRunner::get( OID oid ) {
         boost::mutex::scoped_lock lk(_staticMutex);
         return _activeRuns[ oid ];
     }

    void BenchRunner::populateStats( BenchRunStats *stats ) {
        _brState.assertFinished();
        stats->reset();
        for ( size_t i = 0; i < _workers.size(); ++i )
            stats->updateFrom( _workers[i]->stats() );
    }

     static void appendAverageMsIfAvailable(
             BSONObjBuilder &buf, const std::string &name, const BenchRunEventCounter &counter) {

         if (counter.getNumEvents() > 0)
             buf.append(name,
                        static_cast<double>(counter.getTotalTimeMicros()) / counter.getNumEvents());
     }

     BSONObj BenchRunner::finish( BenchRunner* runner ) {

         runner->stop();

         BenchRunStats stats;
         runner->populateStats(&stats);

         // vector<BSONOBj> errors = runner->config.errors;
         bool error = stats.error;

         if ( error )
             return BSON( "err" << 1 );

         // compute actual ops/sec
         BSONObj before = runner->before["opcounters"].Obj();
         BSONObj after = runner->after["opcounters"].Obj();

         BSONObjBuilder buf;
         buf.append( "note" , "values per second" );
         buf.append( "errCount", (long long) stats.errCount );
         buf.append( "trapped", "error: not implemented" );
         appendAverageMsIfAvailable(buf, "findOneLatencyAverageMs", stats.findOneCounter);
         appendAverageMsIfAvailable(buf, "insertLatencyAverageMs", stats.insertCounter);
         appendAverageMsIfAvailable(buf, "deleteLatencyAverageMs", stats.deleteCounter);
         appendAverageMsIfAvailable(buf, "updateLatencyAverageMs", stats.updateCounter);
         appendAverageMsIfAvailable(buf, "queryLatencyAverageMs", stats.queryCounter);

         {
             BSONObjIterator i( after );
             while ( i.more() ) {
                 BSONElement e = i.next();
                 double x = e.number();
                 x -= before[e.fieldName()].number();
                 buf.append( e.fieldName() , x / runner->_config->seconds );
             }
         }

         BSONObj zoo = buf.obj();

         delete runner;
         return zoo;
     }

     boost::mutex BenchRunner::_staticMutex;
     map< OID, BenchRunner* > BenchRunner::_activeRuns;

     /**
      * benchRun( { ops : [] , host : XXX , db : XXXX , parallel : 5 , seconds : 5 }
      */
     BSONObj benchRunSync( const BSONObj& argsFake, void* data ) {

         BSONObj start = benchStart( argsFake, data );

         OID oid = OID( start.firstElement().String() );
         BenchRunner* runner = BenchRunner::get( oid );
         sleepmillis( (int)(1000.0 * runner->config().seconds) );

         return benchFinish( start, data );
     }

     /**
      * benchRun( { ops : [] , host : XXX , db : XXXX , parallel : 5 , seconds : 5 }
      */
     BSONObj benchStart( const BSONObj& argsFake, void* data ) {

         verify( argsFake.firstElement().isABSONObj() );
         BSONObj args = argsFake.firstElement().Obj();

         // Get new BenchRunner object
         BenchRunner* runner = BenchRunner::createWithConfig( args );

         runner->start();
         return BSON( "" << runner->oid().toString() );
    }

    /**
     * benchRun( { ops : [] , host : XXX , db : XXXX , parallel : 5 , seconds : 5 }
     */
    BSONObj benchFinish( const BSONObj& argsFake, void* data ) {

        OID oid = OID( argsFake.firstElement().String() );

        // Get new BenchRunner object
        BenchRunner* runner = BenchRunner::get( oid );

        BSONObj finalObj = BenchRunner::finish( runner );

        return BSON( "" << finalObj );
    }

    void installBenchmarkSystem( Scope& scope ) {
        scope.injectNative( "benchRun" , benchRunSync );
        scope.injectNative( "benchRunSync" , benchRunSync );
        scope.injectNative( "benchStart" , benchStart );
        scope.injectNative( "benchFinish" , benchFinish );
    }

}
