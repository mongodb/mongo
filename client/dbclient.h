/** @file dbclient.h - connect to a Mongo database as a database, from C++ */

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

#include "../pch.h"
#include "../util/message.h"
#include "../db/jsobj.h"
#include "../db/json.h"
#include <stack>

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
        
        QueryOption_AllSupported = QueryOption_CursorTailable | QueryOption_SlaveOk | QueryOption_OplogReplay | QueryOption_NoCursorTimeout | QueryOption_AwaitData | QueryOption_Exhaust

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

    class DBClientBase;

    class ConnectionString {
    public:
        enum ConnectionType { INVALID , MASTER , PAIR , SET , SYNC };
        
        ConnectionString( const HostAndPort& server ){
            _type = MASTER;
            _servers.push_back( server );
            _finishInit();
        }

        // TODO Delete if nobody is using
        //ConnectionString( ConnectionType type , const vector<HostAndPort>& servers )
        //    : _type( type ) , _servers( servers ){
        //    _finishInit();
        //}
        
        ConnectionString( ConnectionType type , const string& s , const string& setName = "" ){
            _type = type;
            _setName = setName;
            _fillServers( s );
            
            switch ( _type ){
            case MASTER:
                assert( _servers.size() == 1 );
                break;
            case SET:
                assert( _setName.size() );
                assert( _servers.size() >= 1 ); // 1 is ok since we can derive
                break;
            case PAIR:
                assert( _servers.size() == 2 );
                break;
            default:
                assert( _servers.size() > 0 );
            }
            
            _finishInit();
        }

        ConnectionString( const string& s , ConnectionType favoredMultipleType ){
            _fillServers( s );
            if ( _servers.size() == 1 ){
                _type = MASTER;
            }
            else {
                _type = favoredMultipleType;
                assert( _type != MASTER );
            }
            _finishInit();
        }

        bool isValid() const { return _type != INVALID; }
        
        string toString() const {
            return _string;
        }
        
        DBClientBase* connect( string& errmsg ) const;

        static ConnectionString parse( const string& url , string& errmsg );
        
        string getSetName() const{
            return _setName;
        }

        vector<HostAndPort> getServers() const {
            return _servers;
        }
        
    private:

        ConnectionString(){
            _type = INVALID;
        }
        
        void _fillServers( string s ){
            string::size_type idx;
            while ( ( idx = s.find( ',' ) ) != string::npos ){
                _servers.push_back( s.substr( 0 , idx ) );
                s = s.substr( idx + 1 );
            }
            _servers.push_back( s );
        }
        
        void _finishInit(){
            stringstream ss;
            if ( _type == SET )
                ss << _setName << "/";
            for ( unsigned i=0; i<_servers.size(); i++ ){
                if ( i > 0 )
                    ss << ",";
                ss << _servers[i].toString();
            }
            _string = ss.str();
        }

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
        Query(const string &json) : 
            obj(fromjson(json)) { }
        Query(const char * json) : 
            obj(fromjson(json)) { }

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
        Query& hint(const string &jsonKeyPatt) { return hint(fromjson(jsonKeyPatt)); }

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
         * if this query has an orderby, hint, or some other field
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
    
/** Typically one uses the QUERY(...) macro to construct a Query object.
    Example: QUERY( "age" << 33 << "school" << "UCLA" )
*/
#define QUERY(x) mongo::Query( BSON(x) )

    /**
       interface that handles communication with the db
     */
    class DBConnector {
    public:
        virtual ~DBConnector() {}
        virtual bool call( Message &toSend, Message &response, bool assertOk=true ) = 0;
        virtual void say( Message &toSend ) = 0;
        virtual void sayPiggyBack( Message &toSend ) = 0;
        virtual void checkResponse( const char* data, int nReturned ) {}

        /* used by QueryOption_Exhaust.  To use that your subclass must implement this. */
        virtual void recv( Message& m ) { assert(false); }

        virtual string getServerAddress() const = 0;
    };

    /**
       The interface that any db connection should implement
     */
    class DBClientInterface : boost::noncopyable {
    public:
        virtual auto_ptr<DBClientCursor> query(const string &ns, Query query, int nToReturn = 0, int nToSkip = 0,
                                               const BSONObj *fieldsToReturn = 0, int queryOptions = 0 , int batchSize = 0 ) = 0;

        /** don't use this - called automatically by DBClientCursor for you */
        virtual auto_ptr<DBClientCursor> getMore( const string &ns, long long cursorId, int nToReturn = 0, int options = 0 ) = 0;
        
        virtual void insert( const string &ns, BSONObj obj ) = 0;
        
        virtual void insert( const string &ns, const vector< BSONObj >& v ) = 0;

        virtual void remove( const string &ns , Query query, bool justOne = 0 ) = 0;

        virtual void update( const string &ns , Query query , BSONObj obj , bool upsert = 0 , bool multi = 0 ) = 0;

        virtual ~DBClientInterface() { }

        /**
           @return a single object that matches the query.  if none do, then the object is empty
           @throws AssertionException
        */
        virtual BSONObj findOne(const string &ns, const Query& query, const BSONObj *fieldsToReturn = 0, int queryOptions = 0);


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
            @param digestPassword if password is plain text, set this to true.  otherwise assumed to be pre-digested
            @return true if successful
        */
        virtual bool auth(const string &dbname, const string &username, const string &pwd, string& errmsg, bool digestPassword = true);

        /** count number of objects in collection ns that match the query criteria specified
            throws UserAssertion if database returns an error
        */
        unsigned long long count(const string &ns, const BSONObj& query = BSONObj(), int options=0 );

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

        /** Get error result from the last operation on this connection. 
            @return error message text, or empty string if no error.
        */
        string getLastError();
		/** Get error result from the last operation on this connection. 
			@return full error object.
		*/
		virtual BSONObj getLastErrorDetailed();

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
        virtual bool dropCollection( const string &ns ){
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
           to enable.  Profiling information is then written to the system.profiling collection, which one can
           then query.
        */
        enum ProfilingLevel {
            ProfileOff = 0,
            ProfileSlow = 1, // log very slow (>100ms) operations
            ProfileAll = 2
            
        };
        bool setDbProfilingLevel(const string &dbname, ProfilingLevel level, BSONObj *info = 0);
        bool getDbProfilingLevel(const string &dbname, ProfilingLevel& level, BSONObj *info = 0);

        /** Run a map/reduce job on the server. 

            See http://www.mongodb.org/display/DOCS/MapReduce

            ns        namespace (db+collection name) of input data
            jsmapf    javascript map function code 
            jsreducef javascript reduce function code. 
            query     optional query filter for the input
            output    optional permanent output collection name.  if not specified server will 
                      generate a temporary collection and return its name.

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
        BSONObj mapreduce(const string &ns, const string &jsmapf, const string &jsreducef, BSONObj query = BSONObj(), const string& output = "");

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

        /**
           
         */
        bool validate( const string &ns , bool scandata=true ){
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
           @param name if not isn't specified, it will be created from the keys (recommended)
           @return whether or not sent message to db.
             should be true on first call, false on subsequent unless resetIndexCache was called
         */
        virtual bool ensureIndex( const string &ns , BSONObj keys , bool unique = false, const string &name = "" );

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

        /** @return the database name portion of an ns string */
        string nsGetDB( const string &ns ){
            string::size_type pos = ns.find( "." );
            if ( pos == string::npos )
                return ns;
            
            return ns.substr( 0 , pos );
        }
        
        /** @return the collection name portion of an ns string */
        string nsGetCollection( const string &ns ){
            string::size_type pos = ns.find( "." );
            if ( pos == string::npos )
                return "";

            return ns.substr( pos + 1 );            
        }

    protected:
        bool isOk(const BSONObj&);
        
        enum QueryOptions availableOptions();
        
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
        DBClientBase(){
            _writeConcern = W_NORMAL;
        }
        
        WriteConcern getWriteConcern() const { return _writeConcern; }
        void setWriteConcern( WriteConcern w ){ _writeConcern = w; }
        
        /** send a query to the database.
         @param ns namespace to query, format is <dbname>.<collectname>[.<collectname>]*
         @param query query to perform on the collection.  this is a BSONObj (binary JSON)
         You may format as
           { query: { ... }, orderby: { ... } }
         to specify a sort order.
         @param nToReturn n to return.  0 = unlimited
         @param nToSkip start with the nth item
         @param fieldsToReturn optional template of which fields to select. if unspecified, returns all fields
         @param queryOptions see options enum at top of this file

         @return    cursor.   0 if error (connection failure)
         @throws AssertionException
        */
        virtual auto_ptr<DBClientCursor> query(const string &ns, Query query, int nToReturn = 0, int nToSkip = 0,
                                               const BSONObj *fieldsToReturn = 0, int queryOptions = 0 , int batchSize = 0 );

        /** don't use this - called automatically by DBClientCursor for you
            @param cursorId id of cursor to retrieve
            @return an handle to a previously allocated cursor
            @throws AssertionException
         */
        virtual auto_ptr<DBClientCursor> getMore( const string &ns, long long cursorId, int nToReturn = 0, int options = 0 );

        /**
           insert an object into the database
         */
        virtual void insert( const string &ns , BSONObj obj );

        /**
           insert a vector of objects into the database
         */
        virtual void insert( const string &ns, const vector< BSONObj >& v );

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

        static int countCommas( const string& s ){
            int n = 0;
            for ( unsigned i=0; i<s.size(); i++ )
                if ( s[i] == ',' )
                    n++;
            return n;
        }

        virtual bool callRead( Message& toSend , Message& response ) = 0;
        // virtual bool callWrite( Message& toSend , Message& response ) = 0; // TODO: add this if needed
        virtual void say( Message& toSend  ) = 0;

        virtual ConnectionString::ConnectionType type() const = 0;

        /** @return true if conn is either equal to or contained in this connection */
        virtual bool isMember( const DBConnector * conn ) const = 0;
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
        DBClientReplicaSet *clientSet;
        boost::scoped_ptr<MessagingPort> p;
        boost::scoped_ptr<SockAddr> server;
        bool failed; // true if some sort of fatal error has ever happened
        bool autoReconnect;
        time_t lastReconnectTry;
        HostAndPort _server; // remember for reconnects
        string _serverString;
        int _port;
        void _checkConnection();
        void checkConnection() { if( failed ) _checkConnection(); }
		map< string, pair<string,string> > authCache;
        double _timeout;
        
        bool _connect( string& errmsg );
    public:

        /**
           @param _autoReconnect if true, automatically reconnect on a connection failure
           @param cp used by DBClientReplicaSet.  You do not need to specify this parameter
           @param timeout tcp timeout in seconds - this is for read/write, not connect.  
           Connect timeout is fixed, but short, at 5 seconds.
         */
        DBClientConnection(bool _autoReconnect=false, DBClientReplicaSet* cp=0, double timeout=0) :
                clientSet(cp), failed(false), autoReconnect(_autoReconnect), lastReconnectTry(0), _timeout(timeout) { }

        /** Connect to a Mongo database server.

           If autoReconnect is true, you can try to use the DBClientConnection even when
           false was returned -- it will try to connect again.

           @param serverHostname host to connect to.  can include port number ( 127.0.0.1 , 127.0.0.1:5555 )
                                 If you use IPv6 you must add a port number ( ::1:27017 )
           @param errmsg any relevant error message will appended to the string
           @deprecated please use HostAndPort
           @return false if fails to connect.
        */
        virtual bool connect(const char * hostname, string& errmsg){
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

        virtual bool auth(const string &dbname, const string &username, const string &pwd, string& errmsg, bool digestPassword = true);

        virtual auto_ptr<DBClientCursor> query(const string &ns, Query query=Query(), int nToReturn = 0, int nToSkip = 0,
                                               const BSONObj *fieldsToReturn = 0, int queryOptions = 0 , int batchSize = 0 ) {
            checkConnection();
            return DBClientBase::query( ns, query, nToReturn, nToSkip, fieldsToReturn, queryOptions , batchSize );
        }

        /** uses QueryOption_Exhaust 
            use DBClientCursorBatchIterator if you want to do items in large blocks, perhpas to avoid granular locking and such.
         */
        unsigned long long query( boost::function<void(const BSONObj&)> f, const string& ns, Query query, const BSONObj *fieldsToReturn = 0, int queryOptions = 0);
        unsigned long long query( boost::function<void(DBClientCursorBatchIterator&)> f, const string& ns, Query query, const BSONObj *fieldsToReturn = 0, int queryOptions = 0);

        /**
           @return true if this connection is currently in a failed state.  When autoreconnect is on, 
                   a connection will transition back to an ok state after reconnecting.
         */
        bool isFailed() const {
            return failed;
        }

        MessagingPort& port() {
            return *p;
        }

        string toStringLong() const {
            stringstream ss;
            ss << _serverString;
            if ( failed ) ss << " failed";
            return ss.str();
        }

        /** Returns the address of the server */
        string toString() {
            return _serverString;
        }
        
        string getServerAddress() const {
            return _serverString;
        }
        
        virtual void killCursor( long long cursorID );

        virtual bool callRead( Message& toSend , Message& response ){
            return call( toSend , response );
        }

        virtual void say( Message &toSend );
        virtual bool call( Message &toSend, Message &response, bool assertOk = true );
        
        virtual ConnectionString::ConnectionType type() const { return ConnectionString::MASTER; }  

        virtual bool isMember( const DBConnector * conn ) const { return this == conn; };

        virtual void checkResponse( const char *data, int nReturned );

    protected:
        friend class SyncClusterConnection;
        virtual void recv( Message& m );
        virtual void sayPiggyBack( Message &toSend );

    };
    
    /** Use this class to connect to a replica set of servers.  The class will manage
       checking for which server in a replica set is master, and do failover automatically.
       
       This can also be used to connect to replica pairs since pairs are a subset of sets
       
	   On a failover situation, expect at least one operation to return an error (throw 
	   an exception) before the failover is complete.  Operations are not retried.
    */
    class DBClientReplicaSet : public DBClientBase {
        string _name;
        DBClientConnection * _currentMaster;
        vector<HostAndPort> _servers;
        vector<DBClientConnection*> _conns;

        
        void _checkMaster();
        DBClientConnection * checkMaster();

    public:
        /** Call connect() after constructing. autoReconnect is always on for DBClientReplicaSet connections. */
        DBClientReplicaSet( const string& name , const vector<HostAndPort>& servers );
        virtual ~DBClientReplicaSet();

        /** Returns false if nomember of the set were reachable, or neither is
           master, although,
           when false returned, you can still try to use this connection object, it will
           try reconnects.
           */
        bool connect();

        /** Authorize.  Authorizes all nodes as needed
        */
        virtual bool auth(const string &dbname, const string &username, const string &pwd, string& errmsg, bool digestPassword = true );

        /** throws userassertion "no master found" */
        virtual
        auto_ptr<DBClientCursor> query(const string &ns, Query query, int nToReturn = 0, int nToSkip = 0,
                                       const BSONObj *fieldsToReturn = 0, int queryOptions = 0 , int batchSize = 0 );

        /** throws userassertion "no master found" */
        virtual
        BSONObj findOne(const string &ns, const Query& query, const BSONObj *fieldsToReturn = 0, int queryOptions = 0);

        /** insert */
        virtual void insert( const string &ns , BSONObj obj ) {
            checkMaster()->insert(ns, obj);
        }

        /** insert multiple objects.  Note that single object insert is asynchronous, so this version 
            is only nominally faster and not worth a special effort to try to use.  */
        virtual void insert( const string &ns, const vector< BSONObj >& v ) {
            checkMaster()->insert(ns, v);
        }

        /** remove */
        virtual void remove( const string &ns , Query obj , bool justOne = 0 ) {
            checkMaster()->remove(ns, obj, justOne);
        }

        /** update */
        virtual void update( const string &ns , Query query , BSONObj obj , bool upsert = 0 , bool multi = 0 ) {
            return checkMaster()->update(ns, query, obj, upsert,multi);
        }
        
        virtual void killCursor( long long cursorID ){
            checkMaster()->killCursor( cursorID );
        }

        string toString();

        /* this is the callback from our underlying connections to notify us that we got a "not master" error.
         */
        void isntMaster() {
            _currentMaster = 0;
        }
        
        string getServerAddress() const;
        
        DBClientConnection& masterConn();
        DBClientConnection& slaveConn();


        virtual bool call( Message &toSend, Message &response, bool assertOk=true ) { return checkMaster()->call( toSend , response , assertOk ); }
        virtual void say( Message &toSend ) { checkMaster()->say( toSend ); }
        virtual bool callRead( Message& toSend , Message& response ){ return checkMaster()->callRead( toSend , response ); }

        virtual ConnectionString::ConnectionType type() const { return ConnectionString::SET; }  

        virtual bool isMember( const DBConnector * conn ) const;

        virtual void checkResponse( const char *data, int nReturned ) { checkMaster()->checkResponse( data , nReturned ); }

    protected:                
        virtual void sayPiggyBack( Message &toSend ) { checkMaster()->say( toSend ); }
        
        bool isFailed() const {
            return _currentMaster == 0 || _currentMaster->isFailed();
        }
    };
    
    /** pings server to check if it's up
     */
    bool serverAlive( const string &uri );

    DBClientBase * createDirectClient();
    
} // namespace mongo

#include "dbclientcursor.h"
#include "undef_macros.h"
