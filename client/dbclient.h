/** @file dbclient.h - connect to a Mongo database as a database, from C++ */

/**
*    Copyright (C) 2008 10gen Inc.
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

#pragma once

#include "../stdafx.h"
#include "../util/message.h"
#include "../db/jsobj.h"
#include "../db/json.h"

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
        Option_CursorTailable = 1 << 1,

        /** allow query of replica slave.  normally these return an error except for namespace "local".
        */
        Option_SlaveOk = 1 << 2,
        
        Option_OplogReplay = 1 << 3,

        /** if there is a cursor, ignore the normal cursor timeout behavior and never time it out
         */
        Option_NoCursorTimeout = 1 << 4
    };

    class BSONObj;

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
            @param scope Context for the javascript object.  List in a BSON object any 
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
        bool isComplex() const;
        
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
#define QUERY(x) Query( BSON(x) )

    /**
       interface that handles communication with the db
     */
    class DBConnector {
    public:
        virtual ~DBConnector() {}
        virtual bool call( Message &toSend, Message &response, bool assertOk=true ) = 0;
        virtual void say( Message &toSend ) = 0;
        virtual void sayPiggyBack( Message &toSend ) = 0;
        virtual void checkResponse( const string &data, int nReturned ) {}
    };

	/** Queries return a cursor object */
    class DBClientCursor : boost::noncopyable {
    public:
		/** if true, safe to call next() */
        bool more();

        /** next
		   @return next object in the result cursor.
           on an error at the remote server, you will get back:
             { $err: <string> }
           if you do not want to handle that yourself, call nextSafe().
        */
        BSONObj next();

		/** throws AssertionException if get back { $err : ... } */
        BSONObj nextSafe() {
            BSONObj o = next();
            BSONElement e = o.firstElement();
            assert( strcmp(e.fieldName(), "$err") != 0 );
            return o;
        }

        /** cursor no longer valid -- use with tailable cursors.
           note you should only rely on this once more() returns false;
           'dead' may be preset yet some data still queued and locally
           available from the dbclientcursor.
        */
        bool isDead() const {
            return cursorId == 0;
        }

        bool tailable() const {
            return (opts & Option_CursorTailable) != 0;
        }
        
        bool hasResultFlag( int flag ){
            return (resultFlags & flag) != 0;
        }

        bool init();

        DBClientCursor( DBConnector *_connector, const string &_ns, BSONObj _query, int _nToReturn,
                        int _nToSkip, const BSONObj *_fieldsToReturn, int queryOptions ) :
                connector(_connector),
                ns(_ns),
                query(_query),
                nToReturn(_nToReturn),
                nToSkip(_nToSkip),
                fieldsToReturn(_fieldsToReturn),
                opts(queryOptions),
                m(new Message()),
                cursorId(),
                nReturned(),
                pos(),
                data(),
                ownCursor_( true ) {
        }
        
        DBClientCursor( DBConnector *_connector, const string &_ns, long long _cursorId, int _nToReturn, int options ) :
                connector(_connector),
                ns(_ns),
                nToReturn( _nToReturn ),
                opts( options ),
                m(new Message()),
                cursorId( _cursorId ),
                nReturned(),
                pos(),
                data(),
                ownCursor_( true ) {
        }            

        virtual ~DBClientCursor();

        long long getCursorId() const { return cursorId; }
        void decouple() { ownCursor_ = false; }
        
    private:
        DBConnector *connector;
        string ns;
        BSONObj query;
        int nToReturn;
        int nToSkip;
        const BSONObj *fieldsToReturn;
        int opts;
        auto_ptr<Message> m;

        int resultFlags;
        long long cursorId;
        int nReturned;
        int pos;
        const char *data;
        void dataReceived();
        void requestMore();
        bool ownCursor_;
    };
    

    /**
       The interface that any db connection should implement
     */
    class DBClientInterface : boost::noncopyable {
    public:
        virtual auto_ptr<DBClientCursor> query(const string &ns, Query query, int nToReturn = 0, int nToSkip = 0,
                                               const BSONObj *fieldsToReturn = 0, int queryOptions = 0) = 0;

        virtual auto_ptr<DBClientCursor> getMore( const string &ns, long long cursorId, int nToReturn = 0, int options = 0 ) = 0;
        
        virtual BSONObj findOne(const string &ns, Query query, const BSONObj *fieldsToReturn = 0, int queryOptions = 0) = 0;

        virtual void insert( const string &ns, BSONObj obj ) = 0;
        
        virtual void insert( const string &ns, const vector< BSONObj >& v ) = 0;

        virtual void remove( const string &ns , Query query, bool justOne = 0 ) = 0;

        virtual void update( const string &ns , Query query , BSONObj obj , bool upsert = 0 ) = 0;

        virtual ~DBClientInterface() { }
    };

    /**
       DB "commands"
       Basically just invocations of connection.$cmd.findOne({...});
    */
    class DBClientWithCommands : public DBClientInterface {
        bool isOk(const BSONObj&);
    public:

        bool simpleCommand(const string &dbname, BSONObj *info, const string &command);

        /** Run a database command.  Database commands are represented as BSON objects.  Common database
           commands have prebuilt helper functions -- see below.  If a helper is not available you can
           directly call runCommand.

           @param dbname database name.  Use "admin" for global administrative commands.
           @param cmd  the command object to execute.  For example, { ismaster : 1 }
           @param info the result object the database returns. Typically has { ok : ..., errmsg : ... } fields
                       set.

           @return true if the command returned "ok".
        */
        bool runCommand(const string &dbname, const BSONObj& cmd, BSONObj &info);

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
        unsigned long long count(const string &ns, const BSONObj& query = BSONObj());

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

           ns:     fully qualified collection name
            size:   desired initial extent size for the collection.
                   Must be <= 1000000000 for normal collections.
        		   For fixed size (capped) collections, this size is the total/max size of the
        		   collection.
           capped: if true, this is a fixed size collection (where old data rolls out).
           max:    maximum number of objects if capped (optional).

           returns true if successful.
        */
        bool createCollection(const string &ns, unsigned size = 0, bool capped = false, int max = 0, BSONObj *info = 0);

        /** Get error result from the last operation on this connection. 
            @return error message text, or empty string if no error.
        */
        string getLastError();
		/** Get error result from the last operation on this connection. 
			@return full error object.
		*/
		BSONObj getLastErrorDetailed();

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

        /** Erase / drop an entire database */
        virtual bool dropDatabase(const string &dbname, BSONObj *info = 0) {
            return simpleCommand(dbname, info, "dropDatabase");
        }

        /** Delete the specified collection. */        
        virtual bool dropCollection( const string &ns ){
            string db = nsGetDB( ns );
            string coll = nsGetCollection( ns );
            uassert( "no collection name", coll.size() );

            BSONObj info;
            
            return runCommand( db.c_str() , BSON( "drop" << coll ) , info );
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

        /** Run javascript code on the database server.
           dbname    database context in which the code runs. The javascript variable 'db' will be assigned
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
         */
        list<string> getDatabaseNames();

        /**
           get a list of all the current collections in db
         */
        list<string> getCollectionNames( const string& db );

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

    };
    
    /**
     abstract class that implements the core db operations
     */
    class DBClientBase : public DBClientWithCommands, public DBConnector {
    public:
        /** send a query to the database.
         ns:            namespace to query, format is <dbname>.<collectname>[.<collectname>]*
         query:         query to perform on the collection.  this is a BSONObj (binary JSON)
         You may format as
           { query: { ... }, order: { ... } }
         to specify a sort order.
         nToReturn:     n to return.  0 = unlimited
         nToSkip:       start with the nth item
         fieldsToReturn:
         optional template of which fields to select. if unspecified, returns all fields
         queryOptions:  see options enum at top of this file

         @return    cursor.   0 if error (connection failure)
         @throws AssertionException
        */
        virtual auto_ptr<DBClientCursor> query(const string &ns, Query query, int nToReturn = 0, int nToSkip = 0,
                                               const BSONObj *fieldsToReturn = 0, int queryOptions = 0);

        /** @param cursorId id of cursor to retrieve
            @return an handle to a previously allocated cursor
            @throws AssertionException
         */
        virtual auto_ptr<DBClientCursor> getMore( const string &ns, long long cursorId, int nToReturn = 0, int options = 0 );
        
        /**
           @return a single object that matches the query.  if none do, then the object is empty
           @throws AssertionException
        */
        virtual BSONObj findOne(const string &ns, Query query, const BSONObj *fieldsToReturn = 0, int queryOptions = 0);
        
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
        virtual void update( const string &ns , Query query , BSONObj obj , bool upsert = 0 );

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
        
        virtual string getServerAddress() const = 0;
        
        /** Erase / drop an entire database */
        virtual bool dropDatabase(const string &dbname, BSONObj *info = 0) {
            bool ret = DBClientWithCommands::dropDatabase( dbname, info );
            resetIndexCache();
            return ret;
        }
        
        /** Delete the specified collection. */        
        virtual bool dropCollection( const string &ns ){
            bool ret = DBClientWithCommands::dropCollection( ns );
            resetIndexCache();
            return ret;
        }        
        
        virtual bool isFailed() const = 0;

    private:
        set<string> _seenIndexes;
    };
    
    class DBClientPaired;
    
    class ConnectException : public UserException { 
    public:
        ConnectException(string msg) : UserException(msg) { }
    };

    /** 
        A basic connection to the database. 
        This is the main entry point for talking to a simple Mongo setup
    */
    class DBClientConnection : public DBClientBase {
        DBClientPaired *clientPaired;
        auto_ptr<MessagingPort> p;
        auto_ptr<SockAddr> server;
        bool failed; // true if some sort of fatal error has ever happened
        bool autoReconnect;
        time_t lastReconnectTry;
        string serverAddress; // remember for reconnects
        void _checkConnection();
        void checkConnection() { if( failed ) _checkConnection(); }
		map< string, pair<string,string> > authCache;
    public:

        /**
           @param _autoReconnect if true, automatically reconnect on a connection failure
           @param cp used by DBClientPaired.  You do not need to specify this parameter
         */
        DBClientConnection(bool _autoReconnect=false,DBClientPaired* cp=0) :
                clientPaired(cp), failed(false), autoReconnect(_autoReconnect), lastReconnectTry(0) { }

        /** Connect to a Mongo database server.

           If autoReconnect is true, you can try to use the DBClientConnection even when
           false was returned -- it will try to connect again.

           @param serverHostname host to connect to.  can include port number ( 127.0.0.1 , 127.0.0.1:5555 )
           @param errmsg any relevant error message will appended to the string
           @return false if fails to connect.
        */
        virtual bool connect(const string &serverHostname, string& errmsg);

        /** Connect to a Mongo database server.  Exception throwing version.
            Throws a UserException if cannot connect.

           If autoReconnect is true, you can try to use the DBClientConnection even when
           false was returned -- it will try to connect again.

           @param serverHostname host to connect to.  can include port number ( 127.0.0.1 , 127.0.0.1:5555 )
        */
        void connect(string serverHostname) { 
            string errmsg;
            if( !connect(serverHostname.c_str(), errmsg) ) 
                throw ConnectException(string("can't connect ") + errmsg);
        }

        virtual bool auth(const string &dbname, const string &username, const string &pwd, string& errmsg, bool digestPassword = true);

        virtual auto_ptr<DBClientCursor> query(const string &ns, Query query, int nToReturn = 0, int nToSkip = 0,
                                               const BSONObj *fieldsToReturn = 0, int queryOptions = 0) {
            checkConnection();
            return DBClientBase::query( ns, query, nToReturn, nToSkip, fieldsToReturn, queryOptions );
        }

        /**
           @return true if this connection is currently in a failed state.  When autoreconnect is on, 
                   a connection will transition back to an ok state after reconnecting.
         */
        bool isFailed() const {
            return failed;
        }

        MessagingPort& port() {
            return *p.get();
        }

        string toStringLong() const {
            stringstream ss;
            ss << serverAddress;
            if ( failed ) ss << " failed";
            return ss.str();
        }

        /** Returns the address of the server */
        string toString() {
            return serverAddress;
        }
        
        string getServerAddress() const {
            return serverAddress;
        }

    protected:
        virtual bool call( Message &toSend, Message &response, bool assertOk = true );
        virtual void say( Message &toSend );
        virtual void sayPiggyBack( Message &toSend );
        virtual void checkResponse( const char *data, int nReturned );
    };

    /** Use this class to connect to a replica pair of servers.  The class will manage
       checking for which server in a replica pair is master, and do failover automatically.

	   On a failover situation, expect at least one operation to return an error (throw 
	   an exception) before the failover is complete.  Operations are not retried.
    */
    class DBClientPaired : public DBClientBase {
        DBClientConnection left,right;
        enum State {
            NotSetL=0,
            NotSetR=1,
            Left, Right
        } master;

        void _checkMaster();
        DBClientConnection& checkMaster();

    public:
        /** Call connect() after constructing. autoReconnect is always on for DBClientPaired connections. */
        DBClientPaired();

        /** Returns false is neither member of the pair were reachable, or neither is
           master, although,
           when false returned, you can still try to use this connection object, it will
           try reconnects.
           */
        bool connect(const string &serverHostname1, const string &serverHostname2);

        /** Connect to a server pair using a host pair string of the form
              hostname[:port],hostname[:port]
              */
        bool connect(string hostpairstring);

        /** Authorize.  Authorizes both sides of the pair as needed. 
        */
        bool auth(const string &dbname, const string &username, const string &pwd, string& errmsg);

        /** throws userassertion "no master found" */
        virtual
        auto_ptr<DBClientCursor> query(const string &ns, Query query, int nToReturn = 0, int nToSkip = 0,
                                       const BSONObj *fieldsToReturn = 0, int queryOptions = 0);

        /** throws userassertion "no master found" */
        virtual
        BSONObj findOne(const string &ns, Query query, const BSONObj *fieldsToReturn = 0, int queryOptions = 0);

        /** insert */
        virtual void insert( const string &ns , BSONObj obj ) {
            checkMaster().insert(ns, obj);
        }

        /** insert multiple objects.  Note that single object insert is asynchronous, so this version 
            is only nominally faster and not worth a special effort to try to use.  */
        virtual void insert( const string &ns, const vector< BSONObj >& v ) {
            checkMaster().insert(ns, v);
        }

        /** remove */
        virtual void remove( const string &ns , Query obj , bool justOne = 0 ) {
            checkMaster().remove(ns, obj, justOne);
        }

        /** update */
        virtual void update( const string &ns , Query query , BSONObj obj , bool upsert = 0 ) {
            return checkMaster().update(ns, query, obj, upsert);
        }
        
        string toString();

        /* this is the callback from our underlying connections to notify us that we got a "not master" error.
         */
        void isntMaster() {
            master = ( ( master == Left ) ? NotSetR : NotSetL );
        }
        
        string getServerAddress() const {
            return left.getServerAddress() + "," + right.getServerAddress();
        }

        DBClientConnection& slaveConn();

        /* TODO - not yet implemented. mongos may need these. */
        virtual bool call( Message &toSend, Message &response, bool assertOk=true ) { assert(false); return false; }
        virtual void say( Message &toSend ) { assert(false); }
        virtual void sayPiggyBack( Message &toSend ) { assert(false); }
        virtual void checkResponse( const char *data, int nReturned ) { assert(false); }
        
        bool isFailed() const {
            // TODO: this really should check isFailed on current master as well
            return master > NotSetR;
        }
    };
    

    DBClientBase * createDirectClient();
    
} // namespace mongo
