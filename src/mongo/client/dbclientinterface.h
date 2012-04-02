/** @file dbclientinterface.h

    Core MongoDB C++ driver interfaces are defined here.
*/

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include "pch.h"

#include "mongo/db/authlevel.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/message_port.h"

namespace mongo {

    /** the query field 'options' can have these bits set: */
    enum QueryOptions {
        /** Tailable means cursor is not closed when the last data is retrieved.  rather, the cursor marks
           the final object's position.  you can resume using the cursor later, from where it was located,
           if more data were received.  Set on dbQuery and dbGetMore.

           like any "latent cursor", the cursor may become invalid at some point -- for example if that
           final object it references were deleted.  Thus, you should be prepared to requery if you get back
           ResultFlag_CursorNotFound.
        */
        QueryOption_CursorTailable = 1 << 1,

        /** allow query of replica slave.  normally these return an error except for namespace "local".
        */
        QueryOption_SlaveOk = 1 << 2,

        // findingStart mode is used to find the first operation of interest when
        // we are scanning through a repl log.  For efficiency in the common case,
        // where the first operation of interest is closer to the tail than the head,
        // we start from the tail of the log and work backwards until we find the
        // first operation of interest.  Then we scan forward from that first operation,
        // actually returning results to the client.  During the findingStart phase,
        // we release the db mutex occasionally to avoid blocking the db process for
        // an extended period of time.
        QueryOption_OplogReplay = 1 << 3,

        /** The server normally times out idle cursors after an inactivy period to prevent excess memory uses
            Set this option to prevent that.
        */
        QueryOption_NoCursorTimeout = 1 << 4,

        /** Use with QueryOption_CursorTailable.  If we are at the end of the data, block for a while rather
            than returning no data. After a timeout period, we do return as normal.
        */
        QueryOption_AwaitData = 1 << 5,

        /** Stream the data down full blast in multiple "more" packages, on the assumption that the client
            will fully read all data queried.  Faster when you are pulling a lot of data and know you want to
            pull it all down.  Note: it is not allowed to not read all the data unless you close the connection.

            Use the query( boost::function<void(const BSONObj&)> f, ... ) version of the connection's query()
            method, and it will take care of all the details for you.
        */
        QueryOption_Exhaust = 1 << 6,

        /** When sharded, this means its ok to return partial results
            Usually we will fail a query if all required shards aren't up
            If this is set, it'll be a partial result set 
         */
        QueryOption_PartialResults = 1 << 7 ,

        QueryOption_AllSupported = QueryOption_CursorTailable | QueryOption_SlaveOk | QueryOption_OplogReplay | QueryOption_NoCursorTimeout | QueryOption_AwaitData | QueryOption_Exhaust | QueryOption_PartialResults

    };

    enum UpdateOptions {
        /** Upsert - that is, insert the item if no matching item is found. */
        UpdateOption_Upsert = 1 << 0,

        /** Update multiple documents (if multiple documents match query expression).
           (Default is update a single document and stop.) */
        UpdateOption_Multi = 1 << 1,

        /** flag from mongo saying this update went everywhere */
        UpdateOption_Broadcast = 1 << 2
    };

    enum RemoveOptions {
        /** only delete one option */
        RemoveOption_JustOne = 1 << 0,

        /** flag from mongo saying this update went everywhere */
        RemoveOption_Broadcast = 1 << 1
    };

    
    /** 
     * need to put in DbMesssage::ReservedOptions as well
     */
    enum InsertOptions {
        /** With muli-insert keep processing inserts if one fails */
        InsertOption_ContinueOnError = 1 << 0
    };

    class DBClientBase;

    /**
     * ConnectionString handles parsing different ways to connect to mongo and determining method
     * samples:
     *    server
     *    server:port
     *    foo/server:port,server:port   SET
     *    server,server,server          SYNC
     *
     * tyipcal use
     * string errmsg,
     * ConnectionString cs = ConnectionString::parse( url , errmsg );
     * if ( ! cs.isValid() ) throw "bad: " + errmsg;
     * DBClientBase * conn = cs.connect( errmsg );
     */
    class ConnectionString {
    public:
        enum ConnectionType { INVALID , MASTER , PAIR , SET , SYNC };

        ConnectionString() {
            _type = INVALID;
        }

        ConnectionString( const HostAndPort& server ) {
            _type = MASTER;
            _servers.push_back( server );
            _finishInit();
        }

        ConnectionString( ConnectionType type , const string& s , const string& setName = "" ) {
            _type = type;
            _setName = setName;
            _fillServers( s );

            switch ( _type ) {
            case MASTER:
                verify( _servers.size() == 1 );
                break;
            case SET:
                verify( _setName.size() );
                verify( _servers.size() >= 1 ); // 1 is ok since we can derive
                break;
            case PAIR:
                verify( _servers.size() == 2 );
                break;
            default:
                verify( _servers.size() > 0 );
            }

            _finishInit();
        }

        ConnectionString( const string& s , ConnectionType favoredMultipleType ) {
            _type = INVALID;
            
            _fillServers( s );
            if ( _type != INVALID ) {
                // set already
            }
            else if ( _servers.size() == 1 ) {
                _type = MASTER;
            }
            else {
                _type = favoredMultipleType;
                verify( _type == SET || _type == SYNC );
            }
            _finishInit();
        }

        bool isValid() const { return _type != INVALID; }

        string toString() const { return _string; }
        
        DBClientBase* connect( string& errmsg, double socketTimeout = 0 ) const;

        string getSetName() const { return _setName; }

        vector<HostAndPort> getServers() const { return _servers; }
        
        ConnectionType type() const { return _type; }

        static ConnectionString parse( const string& url , string& errmsg );

        static string typeToString( ConnectionType type );

    private:

        void _fillServers( string s );
        void _finishInit();

        ConnectionType _type;
        vector<HostAndPort> _servers;
        string _string;
        string _setName;
    };

    /**
     * controls how much a clients cares about writes
     * default is NORMAL
     */
    enum WriteConcern {
        W_NONE = 0 , // TODO: not every connection type fully supports this
        W_NORMAL = 1
        // TODO SAFE = 2
    };

    class BSONObj;
    class ScopedDbConnection;
    class DBClientCursor;
    class DBClientCursorBatchIterator;

    /** Represents a Mongo query expression.  Typically one uses the QUERY(...) macro to construct a Query object.
        Examples:
           QUERY( "age" << 33 << "school" << "UCLA" ).sort("name")
           QUERY( "age" << GT << 30 << LT << 50 )
    */
    class Query {
    public:
        BSONObj obj;
        Query() : obj(BSONObj()) { }
        Query(const BSONObj& b) : obj(b) { }
        Query(const string &json);
        Query(const char * json);

        /** Add a sort (ORDER BY) criteria to the query expression.
            @param sortPattern the sort order template.  For example to order by name ascending, time descending:
              { name : 1, ts : -1 }
            i.e.
              BSON( "name" << 1 << "ts" << -1 )
            or
              fromjson(" name : 1, ts : -1 ")
        */
        Query& sort(const BSONObj& sortPattern);

        /** Add a sort (ORDER BY) criteria to the query expression.
            This version of sort() assumes you want to sort on a single field.
            @param asc = 1 for ascending order
            asc = -1 for descending order
        */
        Query& sort(const string &field, int asc = 1) { sort( BSON( field << asc ) ); return *this; }

        /** Provide a hint to the query.
            @param keyPattern Key pattern for the index to use.
            Example:
              hint("{ts:1}")
        */
        Query& hint(BSONObj keyPattern);
        Query& hint(const string &jsonKeyPatt);

        /** Provide min and/or max index limits for the query.
            min <= x < max
         */
        Query& minKey(const BSONObj &val);
        /**
           max is exclusive
         */
        Query& maxKey(const BSONObj &val);

        /** Return explain information about execution of this query instead of the actual query results.
            Normally it is easier to use the mongo shell to run db.find(...).explain().
        */
        Query& explain();

        /** Use snapshot mode for the query.  Snapshot mode assures no duplicates are returned, or objects missed, which were
            present at both the start and end of the query's execution (if an object is new during the query, or deleted during
            the query, it may or may not be returned, even with snapshot mode).

            Note that short query responses (less than 1MB) are always effectively snapshotted.

            Currently, snapshot mode may not be used with sorting or explicit hints.
        */
        Query& snapshot();

        /** Queries to the Mongo database support a $where parameter option which contains
            a javascript function that is evaluated to see whether objects being queried match
            its criteria.  Use this helper to append such a function to a query object.
            Your query may also contain other traditional Mongo query terms.

            @param jscode The javascript function to evaluate against each potential object
                   match.  The function must return true for matched objects.  Use the this
                   variable to inspect the current object.
            @param scope SavedContext for the javascript object.  List in a BSON object any
                   variables you would like defined when the jscode executes.  One can think
                   of these as "bind variables".

            Examples:
              conn.findOne("test.coll", Query("{a:3}").where("this.b == 2 || this.c == 3"));
              Query badBalance = Query().where("this.debits - this.credits < 0");
        */
        Query& where(const string &jscode, BSONObj scope);
        Query& where(const string &jscode) { return where(jscode, BSONObj()); }

        /**
         * @return true if this query has an orderby, hint, or some other field
         */
        bool isComplex( bool * hasDollar = 0 ) const;

        BSONObj getFilter() const;
        BSONObj getSort() const;
        BSONObj getHint() const;
        bool isExplain() const;

        string toString() const;
        operator string() const { return toString(); }
    private:
        void makeComplex();
        template< class T >
        void appendComplex( const char *fieldName, const T& val ) {
            makeComplex();
            BSONObjBuilder b;
            b.appendElements(obj);
            b.append(fieldName, val);
            obj = b.obj();
        }
    };

    /**
     * Represents a full query description, including all options required for the query to be passed on
     * to other hosts
     */
    class QuerySpec {

        string _ns;
        int _ntoskip;
        int _ntoreturn;
        int _options;
        BSONObj _query;
        BSONObj _fields;
        Query _queryObj;

    public:
        
        QuerySpec( const string& ns,
                   const BSONObj& query, const BSONObj& fields,
                   int ntoskip, int ntoreturn, int options )
            : _ns( ns ), _ntoskip( ntoskip ), _ntoreturn( ntoreturn ), _options( options ),
              _query( query.getOwned() ), _fields( fields.getOwned() ) , _queryObj( _query ) {
        }

        QuerySpec() {}

        bool isEmpty() const { return _ns.size() == 0; }

        bool isExplain() const { return _queryObj.isExplain(); }
        BSONObj filter() const { return _queryObj.getFilter(); }

        BSONObj hint() const { return _queryObj.getHint(); }
        BSONObj sort() const { return _queryObj.getSort(); }
        BSONObj query() const { return _query; }
        BSONObj fields() const { return _fields; }
        BSONObj* fieldsData() { return &_fields; }

        // don't love this, but needed downstrem
        const BSONObj* fieldsPtr() const { return &_fields; } 

        string ns() const { return _ns; }
        int ntoskip() const { return _ntoskip; }
        int ntoreturn() const { return _ntoreturn; }
        int options() const { return _options; }
        
        void setFields( BSONObj& o ) { _fields = o.getOwned(); }

        string toString() const {
            return str::stream() << "QSpec " << 
                BSON( "ns" << _ns << "n2skip" << _ntoskip << "n2return" << _ntoreturn << "options" << _options
                      << "query" << _query << "fields" << _fields );
        }
        
    };


    /** Typically one uses the QUERY(...) macro to construct a Query object.
        Example: QUERY( "age" << 33 << "school" << "UCLA" )
    */
#define QUERY(x) mongo::Query( BSON(x) )

    // Useful utilities for namespaces
    /** @return the database name portion of an ns string */
    string nsGetDB( const string &ns );

    /** @return the collection name portion of an ns string */
    string nsGetCollection( const string &ns );

    /**
       interface that handles communication with the db
     */
    class DBConnector {
    public:
        virtual ~DBConnector() {}
        /** actualServer is set to the actual server where they call went if there was a choice (SlaveOk) */
        virtual bool call( Message &toSend, Message &response, bool assertOk=true , string * actualServer = 0 ) = 0;
        virtual void say( Message &toSend, bool isRetry = false , string * actualServer = 0 ) = 0;
        virtual void sayPiggyBack( Message &toSend ) = 0;
        /* used by QueryOption_Exhaust.  To use that your subclass must implement this. */
        virtual bool recv( Message& m ) { verify(false); return false; }
        // In general, for lazy queries, we'll need to say, recv, then checkResponse
        virtual void checkResponse( const char* data, int nReturned, bool* retry = NULL, string* targetHost = NULL ) {
            if( retry ) *retry = false; if( targetHost ) *targetHost = "";
        }
        virtual bool lazySupported() const = 0;
    };

    /**
       The interface that any db connection should implement
     */
    class DBClientInterface : boost::noncopyable {
    public:
        virtual auto_ptr<DBClientCursor> query(const string &ns, Query query, int nToReturn = 0, int nToSkip = 0,
                                               const BSONObj *fieldsToReturn = 0, int queryOptions = 0 , int batchSize = 0 ) = 0;

        virtual void insert( const string &ns, BSONObj obj , int flags=0) = 0;

        virtual void insert( const string &ns, const vector< BSONObj >& v , int flags=0) = 0;

        virtual void remove( const string &ns , Query query, bool justOne = 0 ) = 0;

        virtual void update( const string &ns , Query query , BSONObj obj , bool upsert = 0 , bool multi = 0 ) = 0;

        virtual ~DBClientInterface() { }

        /**
           @return a single object that matches the query.  if none do, then the object is empty
           @throws AssertionException
        */
        virtual BSONObj findOne(const string &ns, const Query& query, const BSONObj *fieldsToReturn = 0, int queryOptions = 0);

        /** query N objects from the database into an array.  makes sense mostly when you want a small number of results.  if a huge number, use 
            query() and iterate the cursor. 
        */
        void findN(vector<BSONObj>& out, const string&ns, Query query, int nToReturn, int nToSkip = 0, const BSONObj *fieldsToReturn = 0, int queryOptions = 0);

        virtual string getServerAddress() const = 0;

        /** don't use this - called automatically by DBClientCursor for you */
        virtual auto_ptr<DBClientCursor> getMore( const string &ns, long long cursorId, int nToReturn = 0, int options = 0 ) = 0;
    };

    /**
       DB "commands"
       Basically just invocations of connection.$cmd.findOne({...});
    */
    class DBClientWithCommands : public DBClientInterface {
        set<string> _seenIndexes;
    public:
        /** controls how chatty the client is about network errors & such.  See log.h */
        int _logLevel;

        DBClientWithCommands() : _logLevel(0), _cachedAvailableOptions( (enum QueryOptions)0 ), _haveCachedAvailableOptions(false) { }

        /** helper function.  run a simple command where the command expression is simply
              { command : 1 }
            @param info -- where to put result object.  may be null if caller doesn't need that info
            @param command -- command name
            @return true if the command returned "ok".
         */
        bool simpleCommand(const string &dbname, BSONObj *info, const string &command);

        /** Run a database command.  Database commands are represented as BSON objects.  Common database
            commands have prebuilt helper functions -- see below.  If a helper is not available you can
            directly call runCommand.

            @param dbname database name.  Use "admin" for global administrative commands.
            @param cmd  the command object to execute.  For example, { ismaster : 1 }
            @param info the result object the database returns. Typically has { ok : ..., errmsg : ... } fields
                   set.
            @param options see enum QueryOptions - normally not needed to run a command
            @return true if the command returned "ok".
        */
        virtual bool runCommand(const string &dbname, const BSONObj& cmd, BSONObj &info, int options=0);

        /** Authorize access to a particular database.
            Authentication is separate for each database on the server -- you may authenticate for any
            number of databases on a single connection.
            The "admin" database is special and once authenticated provides access to all databases on the
            server.
            @param      digestPassword  if password is plain text, set this to true.  otherwise assumed to be pre-digested
            @param[out] authLevel       level of authentication for the given user
            @return true if successful
        */
        virtual bool auth(const string &dbname, const string &username, const string &pwd, string& errmsg, bool digestPassword = true, Auth::Level * level = NULL);

        /** count number of objects in collection ns that match the query criteria specified
            throws UserAssertion if database returns an error
        */
        virtual unsigned long long count(const string &ns, const BSONObj& query = BSONObj(), int options=0, int limit=0, int skip=0 );

        string createPasswordDigest( const string &username , const string &clearTextPassword );

        /** returns true in isMaster parm if this db is the current master
           of a replica pair.

           pass in info for more details e.g.:
             { "ismaster" : 1.0 , "msg" : "not paired" , "ok" : 1.0  }

           returns true if command invoked successfully.
        */
        virtual bool isMaster(bool& isMaster, BSONObj *info=0);

        /**
           Create a new collection in the database.  Normally, collection creation is automatic.  You would
           use this function if you wish to specify special options on creation.

           If the collection already exists, no action occurs.

           @param ns     fully qualified collection name
           @param size   desired initial extent size for the collection.
                         Must be <= 1000000000 for normal collections.
                         For fixed size (capped) collections, this size is the total/max size of the
                         collection.
           @param capped if true, this is a fixed size collection (where old data rolls out).
           @param max    maximum number of objects if capped (optional).

           returns true if successful.
        */
        bool createCollection(const string &ns, long long size = 0, bool capped = false, int max = 0, BSONObj *info = 0);

        /** Get error result from the last write operation (insert/update/delete) on this connection.
            @return error message text, or empty string if no error.
        */
        string getLastError(bool fsync = false, bool j = false, int w = 0, int wtimeout = 0);

        /** Get error result from the last write operation (insert/update/delete) on this connection.
            @return full error object.

            If "w" is -1, wait for propagation to majority of nodes.
            If "wtimeout" is 0, the operation will block indefinitely if needed.
        */
        virtual BSONObj getLastErrorDetailed(bool fsync = false, bool j = false, int w = 0, int wtimeout = 0);

        /** Can be called with the returned value from getLastErrorDetailed to extract an error string. 
            If all you need is the string, just call getLastError() instead.
        */
        static string getLastErrorString( const BSONObj& res );

        /** Return the last error which has occurred, even if not the very last operation.

           @return { err : <error message>, nPrev : <how_many_ops_back_occurred>, ok : 1 }

           result.err will be null if no error has occurred.
        */
        BSONObj getPrevError();

        /** Reset the previous error state for this connection (accessed via getLastError and
            getPrevError).  Useful when performing several operations at once and then checking
            for an error after attempting all operations.
        */
        bool resetError() { return simpleCommand("admin", 0, "reseterror"); }

        /** Delete the specified collection. */
        virtual bool dropCollection( const string &ns ) {
            string db = nsGetDB( ns );
            string coll = nsGetCollection( ns );
            uassert( 10011 ,  "no collection name", coll.size() );

            BSONObj info;

            bool res = runCommand( db.c_str() , BSON( "drop" << coll ) , info );
            resetIndexCache();
            return res;
        }

        /** Perform a repair and compaction of the specified database.  May take a long time to run.  Disk space
           must be available equal to the size of the database while repairing.
        */
        bool repairDatabase(const string &dbname, BSONObj *info = 0) {
            return simpleCommand(dbname, info, "repairDatabase");
        }

        /** Copy database from one server or name to another server or name.

           Generally, you should dropDatabase() first as otherwise the copied information will MERGE
           into whatever data is already present in this database.

           For security reasons this function only works when you are authorized to access the "admin" db.  However,
           if you have access to said db, you can copy any database from one place to another.
           TODO: this needs enhancement to be more flexible in terms of security.

           This method provides a way to "rename" a database by copying it to a new db name and
           location.  The copy is "repaired" and compacted.

           fromdb   database name from which to copy.
           todb     database name to copy to.
           fromhost hostname of the database (and optionally, ":port") from which to
                    copy the data.  copies from self if "".

           returns true if successful
        */
        bool copyDatabase(const string &fromdb, const string &todb, const string &fromhost = "", BSONObj *info = 0);

        /** The Mongo database provides built-in performance profiling capabilities.  Uset setDbProfilingLevel()
           to enable.  Profiling information is then written to the system.profile collection, which one can
           then query.
        */
        enum ProfilingLevel {
            ProfileOff = 0,
            ProfileSlow = 1, // log very slow (>100ms) operations
            ProfileAll = 2

        };
        bool setDbProfilingLevel(const string &dbname, ProfilingLevel level, BSONObj *info = 0);
        bool getDbProfilingLevel(const string &dbname, ProfilingLevel& level, BSONObj *info = 0);


        /** This implicitly converts from char*, string, and BSONObj to be an argument to mapreduce
            You shouldn't need to explicitly construct this
         */
        struct MROutput {
            MROutput(const char* collection) : out(BSON("replace" << collection)) {}
            MROutput(const string& collection) : out(BSON("replace" << collection)) {}
            MROutput(const BSONObj& obj) : out(obj) {}

            BSONObj out;
        };
        static MROutput MRInline;

        /** Run a map/reduce job on the server.

            See http://www.mongodb.org/display/DOCS/MapReduce

            ns        namespace (db+collection name) of input data
            jsmapf    javascript map function code
            jsreducef javascript reduce function code.
            query     optional query filter for the input
            output    either a string collection name or an object representing output type
                      if not specified uses inline output type

            returns a result object which contains:
             { result : <collection_name>,
               numObjects : <number_of_objects_scanned>,
               timeMillis : <job_time>,
               ok : <1_if_ok>,
               [, err : <errmsg_if_error>]
             }

             For example one might call:
               result.getField("ok").trueValue()
             on the result to check if ok.
        */
        BSONObj mapreduce(const string &ns, const string &jsmapf, const string &jsreducef, BSONObj query = BSONObj(), MROutput output = MRInline);

        /** Run javascript code on the database server.
           dbname    database SavedContext in which the code runs. The javascript variable 'db' will be assigned
                     to this database when the function is invoked.
           jscode    source code for a javascript function.
           info      the command object which contains any information on the invocation result including
                      the return value and other information.  If an error occurs running the jscode, error
                     information will be in info.  (try "out() << info.toString()")
           retValue  return value from the jscode function.
           args      args to pass to the jscode function.  when invoked, the 'args' variable will be defined
                     for use by the jscode.

           returns true if runs ok.

           See testDbEval() in dbclient.cpp for an example of usage.
        */
        bool eval(const string &dbname, const string &jscode, BSONObj& info, BSONElement& retValue, BSONObj *args = 0);

        /** validate a collection, checking for errors and reporting back statistics.
            this operation is slow and blocking.
         */
        bool validate( const string &ns , bool scandata=true ) {
            BSONObj cmd = BSON( "validate" << nsGetCollection( ns ) << "scandata" << scandata );
            BSONObj info;
            return runCommand( nsGetDB( ns ).c_str() , cmd , info );
        }

        /* The following helpers are simply more convenient forms of eval() for certain common cases */

        /* invocation with no return value of interest -- with or without one simple parameter */
        bool eval(const string &dbname, const string &jscode);
        template< class T >
        bool eval(const string &dbname, const string &jscode, T parm1) {
            BSONObj info;
            BSONElement retValue;
            BSONObjBuilder b;
            b.append("0", parm1);
            BSONObj args = b.done();
            return eval(dbname, jscode, info, retValue, &args);
        }

        /** eval invocation with one parm to server and one numeric field (either int or double) returned */
        template< class T, class NumType >
        bool eval(const string &dbname, const string &jscode, T parm1, NumType& ret) {
            BSONObj info;
            BSONElement retValue;
            BSONObjBuilder b;
            b.append("0", parm1);
            BSONObj args = b.done();
            if ( !eval(dbname, jscode, info, retValue, &args) )
                return false;
            ret = (NumType) retValue.number();
            return true;
        }

        /**
           get a list of all the current databases
           uses the { listDatabases : 1 } command.
           throws on error
         */
        list<string> getDatabaseNames();

        /**
           get a list of all the current collections in db
         */
        list<string> getCollectionNames( const string& db );

        bool exists( const string& ns );

        /** Create an index if it does not already exist.
            ensureIndex calls are remembered so it is safe/fast to call this function many
            times in your code.
           @param ns collection to be indexed
           @param keys the "key pattern" for the index.  e.g., { name : 1 }
           @param unique if true, indicates that key uniqueness should be enforced for this index
           @param name if not specified, it will be created from the keys automatically (which is recommended)
           @param cache if set to false, the index cache for the connection won't remember this call
           @param background build index in the background (see mongodb docs/wiki for details)
           @param v index version. leave at default value. (unit tests set this parameter.)
           @return whether or not sent message to db.
             should be true on first call, false on subsequent unless resetIndexCache was called
         */
        virtual bool ensureIndex( const string &ns , BSONObj keys , bool unique = false, const string &name = "",
                                  bool cache = true, bool background = false, int v = -1 );

        /**
           clears the index cache, so the subsequent call to ensureIndex for any index will go to the server
         */
        virtual void resetIndexCache();

        virtual auto_ptr<DBClientCursor> getIndexes( const string &ns );

        virtual void dropIndex( const string& ns , BSONObj keys );
        virtual void dropIndex( const string& ns , const string& indexName );

        /**
           drops all indexes for the collection
         */
        virtual void dropIndexes( const string& ns );

        virtual void reIndex( const string& ns );

        string genIndexName( const BSONObj& keys );

        /** Erase / drop an entire database */
        virtual bool dropDatabase(const string &dbname, BSONObj *info = 0) {
            bool ret = simpleCommand(dbname, info, "dropDatabase");
            resetIndexCache();
            return ret;
        }

        virtual string toString() = 0;

    protected:
        /** if the result of a command is ok*/
        bool isOk(const BSONObj&);

        /** if the element contains a not master error */
        bool isNotMasterErrorString( const BSONElement& e );

        BSONObj _countCmd(const string &ns, const BSONObj& query, int options, int limit, int skip );

        /**
         * Look up the options available on this client.  Caches the answer from
         * _lookupAvailableOptions(), below.
         */
        QueryOptions availableOptions();

        virtual QueryOptions _lookupAvailableOptions();

    private:
        enum QueryOptions _cachedAvailableOptions;
        bool _haveCachedAvailableOptions;
    };

    /**
     abstract class that implements the core db operations
     */
    class DBClientBase : public DBClientWithCommands, public DBConnector {
    protected:
        WriteConcern _writeConcern;

    public:
        DBClientBase() {
            _writeConcern = W_NORMAL;
        }

        WriteConcern getWriteConcern() const { return _writeConcern; }
        void setWriteConcern( WriteConcern w ) { _writeConcern = w; }

        /** send a query to the database.
         @param ns namespace to query, format is <dbname>.<collectname>[.<collectname>]*
         @param query query to perform on the collection.  this is a BSONObj (binary JSON)
         You may format as
           { query: { ... }, orderby: { ... } }
         to specify a sort order.
         @param nToReturn n to return (i.e., limit).  0 = unlimited
         @param nToSkip start with the nth item
         @param fieldsToReturn optional template of which fields to select. if unspecified, returns all fields
         @param queryOptions see options enum at top of this file

         @return    cursor.   0 if error (connection failure)
         @throws AssertionException
        */
        virtual auto_ptr<DBClientCursor> query(const string &ns, Query query, int nToReturn = 0, int nToSkip = 0,
                                               const BSONObj *fieldsToReturn = 0, int queryOptions = 0 , int batchSize = 0 );


        /** Uses QueryOption_Exhaust, when available.

            Exhaust mode sends back all data queries as fast as possible, with no back-and-forth for
            OP_GETMORE.  If you are certain you will exhaust the query, it could be useful.

            Use the DBClientCursorBatchIterator version, below, if you want to do items in large
            blocks, perhaps to avoid granular locking and such.
         */
        virtual unsigned long long query( boost::function<void(const BSONObj&)> f,
                                          const string& ns,
                                          Query query,
                                          const BSONObj *fieldsToReturn = 0,
                                          int queryOptions = 0 );

        virtual unsigned long long query( boost::function<void(DBClientCursorBatchIterator&)> f,
                                          const string& ns,
                                          Query query,
                                          const BSONObj *fieldsToReturn = 0,
                                          int queryOptions = 0 );


        /** don't use this - called automatically by DBClientCursor for you
            @param cursorId id of cursor to retrieve
            @return an handle to a previously allocated cursor
            @throws AssertionException
         */
        virtual auto_ptr<DBClientCursor> getMore( const string &ns, long long cursorId, int nToReturn = 0, int options = 0 );

        /**
           insert an object into the database
         */
        virtual void insert( const string &ns , BSONObj obj , int flags=0);

        /**
           insert a vector of objects into the database
         */
        virtual void insert( const string &ns, const vector< BSONObj >& v , int flags=0);

        /**
           remove matching objects from the database
           @param justOne if this true, then once a single match is found will stop
         */
        virtual void remove( const string &ns , Query q , bool justOne = 0 );

        /**
           updates objects matching query
         */
        virtual void update( const string &ns , Query query , BSONObj obj , bool upsert = false , bool multi = false );

        virtual bool isFailed() const = 0;

        virtual void killCursor( long long cursorID ) = 0;

        virtual bool callRead( Message& toSend , Message& response ) = 0;
        // virtual bool callWrite( Message& toSend , Message& response ) = 0; // TODO: add this if needed
        
        virtual ConnectionString::ConnectionType type() const = 0;
        
        virtual double getSoTimeout() const = 0;

    }; // DBClientBase

    class DBClientReplicaSet;

    class ConnectException : public UserException {
    public:
        ConnectException(string msg) : UserException(9000,msg) { }
    };

    /**
        A basic connection to the database.
        This is the main entry point for talking to a simple Mongo setup
    */
    class DBClientConnection : public DBClientBase {
    public:
        using DBClientBase::query;

        /**
           @param _autoReconnect if true, automatically reconnect on a connection failure
           @param cp used by DBClientReplicaSet.  You do not need to specify this parameter
           @param timeout tcp timeout in seconds - this is for read/write, not connect.
           Connect timeout is fixed, but short, at 5 seconds.
         */
        DBClientConnection(bool _autoReconnect=false, DBClientReplicaSet* cp=0, double so_timeout=0) :
            clientSet(cp), _failed(false), autoReconnect(_autoReconnect), lastReconnectTry(0), _so_timeout(so_timeout) {
            _numConnections++;
        }

        virtual ~DBClientConnection() {
            _numConnections--;
        }

        /** Connect to a Mongo database server.

           If autoReconnect is true, you can try to use the DBClientConnection even when
           false was returned -- it will try to connect again.

           @param serverHostname host to connect to.  can include port number ( 127.0.0.1 , 127.0.0.1:5555 )
                                 If you use IPv6 you must add a port number ( ::1:27017 )
           @param errmsg any relevant error message will appended to the string
           @deprecated please use HostAndPort
           @return false if fails to connect.
        */
        virtual bool connect(const char * hostname, string& errmsg) {
            // TODO: remove this method
            HostAndPort t( hostname );
            return connect( t , errmsg );
        }

        /** Connect to a Mongo database server.

           If autoReconnect is true, you can try to use the DBClientConnection even when
           false was returned -- it will try to connect again.

           @param server server to connect to.
           @param errmsg any relevant error message will appended to the string
           @return false if fails to connect.
        */
        virtual bool connect(const HostAndPort& server, string& errmsg);

        /** Connect to a Mongo database server.  Exception throwing version.
            Throws a UserException if cannot connect.

           If autoReconnect is true, you can try to use the DBClientConnection even when
           false was returned -- it will try to connect again.

           @param serverHostname host to connect to.  can include port number ( 127.0.0.1 , 127.0.0.1:5555 )
        */
        void connect(const string& serverHostname) {
            string errmsg;
            if( !connect(HostAndPort(serverHostname), errmsg) )
                throw ConnectException(string("can't connect ") + errmsg);
        }

        virtual bool auth(const string &dbname, const string &username, const string &pwd, string& errmsg, bool digestPassword = true, Auth::Level* level=NULL);

        virtual auto_ptr<DBClientCursor> query(const string &ns, Query query=Query(), int nToReturn = 0, int nToSkip = 0,
                                               const BSONObj *fieldsToReturn = 0, int queryOptions = 0 , int batchSize = 0 ) {
            checkConnection();
            return DBClientBase::query( ns, query, nToReturn, nToSkip, fieldsToReturn, queryOptions , batchSize );
        }

        virtual unsigned long long query( boost::function<void(DBClientCursorBatchIterator &)> f,
                                          const string& ns,
                                          Query query,
                                          const BSONObj *fieldsToReturn,
                                          int queryOptions );

        virtual bool runCommand(const string &dbname, const BSONObj& cmd, BSONObj &info, int options=0);

        /**
           @return true if this connection is currently in a failed state.  When autoreconnect is on,
                   a connection will transition back to an ok state after reconnecting.
         */
        bool isFailed() const { return _failed; }

        MessagingPort& port() { verify(p); return *p; }

        string toStringLong() const {
            stringstream ss;
            ss << _serverString;
            if ( _failed ) ss << " failed";
            return ss.str();
        }

        /** Returns the address of the server */
        string toString() { return _serverString; }

        string getServerAddress() const { return _serverString; }

        virtual void killCursor( long long cursorID );
        virtual bool callRead( Message& toSend , Message& response ) { return call( toSend , response ); }
        virtual void say( Message &toSend, bool isRetry = false , string * actualServer = 0 );
        virtual bool recv( Message& m );
        virtual void checkResponse( const char *data, int nReturned, bool* retry = NULL, string* host = NULL );
        virtual bool call( Message &toSend, Message &response, bool assertOk = true , string * actualServer = 0 );
        virtual ConnectionString::ConnectionType type() const { return ConnectionString::MASTER; }
        void setSoTimeout(double to) { _so_timeout = to; }
        double getSoTimeout() const { return _so_timeout; }

        virtual bool lazySupported() const { return true; }

        static int getNumConnections() {
            return _numConnections;
        }
        
        static void setLazyKillCursor( bool lazy ) { _lazyKillCursor = lazy; }
        static bool getLazyKillCursor() { return _lazyKillCursor; }
        
    protected:
        friend class SyncClusterConnection;
        virtual void sayPiggyBack( Message &toSend );

        DBClientReplicaSet *clientSet;
        boost::scoped_ptr<MessagingPort> p;
        boost::scoped_ptr<SockAddr> server;
        bool _failed;
        const bool autoReconnect;
        time_t lastReconnectTry;
        HostAndPort _server; // remember for reconnects
        string _serverString;
        void _checkConnection();

        // throws SocketException if in failed state and not reconnecting or if waiting to reconnect
        void checkConnection() { if( _failed ) _checkConnection(); }

        map< string, pair<string,string> > authCache;
        double _so_timeout;
        bool _connect( string& errmsg );

        static AtomicUInt _numConnections;
        static bool _lazyKillCursor; // lazy means we piggy back kill cursors on next op

#ifdef MONGO_SSL
        static SSLManager* sslManager();
        static SSLManager* _sslManager;
#endif
    };

    /** pings server to check if it's up
     */
    bool serverAlive( const string &uri );

    DBClientBase * createDirectClient();

    BSONElement getErrField( const BSONObj& result );
    bool hasErrField( const BSONObj& result );

    inline std::ostream& operator<<( std::ostream &s, const Query &q ) {
        return s << q.toString();
    }

} // namespace mongo

#include "mongo/client/dbclientcursor.h"
