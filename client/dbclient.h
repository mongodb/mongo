// dbclient.h - connect to a Mongo database as a database, from C++

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
#include "../grid/message.h"
#include "../db/jsobj.h"

namespace mongo {

    /* the query field 'options' can have these bits set: */
    enum QueryOptions {
        /* Tailable means cursor is not closed when the last data is retrieved.  rather, the cursor marks
           the final object's position.  you can resume using the cursor later, from where it was located,
           if more data were received.  Set on dbQuery and dbGetMore.

           like any "latent cursor", the cursor may become invalid at some point -- for example if that
           final object it references were deleted.  Thus, you should be prepared to requery if you get back
           ResultFlag_CursorNotFound.
        */
        Option_CursorTailable = 2,

        /* allow query of replica slave.  normally these return an error except for namespace "local".
        */
        Option_SlaveOk = 4,

        Option_ALLMASK = 6
    };

    class BSONObj;

    /* db response format

       Query or GetMore: // see struct QueryResult
          int resultFlags;
          int64 cursorID;
          int startingFrom;
          int nReturned;
          list of marshalled JSObjects;
    */

#pragma pack(push,1)
    struct QueryResult : public MsgData {
        enum {
            ResultFlag_CursorNotFound = 1, /* returned, with zero results, when getMore is called but the cursor id is not valid at the server. */
            ResultFlag_ErrSet = 2          /* { $err : ... } is being returned */
        };

        long long cursorId;
        int startingFrom;
        int nReturned;
        const char *data() {
            return (char *) (((int *)&nReturned)+1);
        }
        int& resultFlags() {
            return dataAsInt();
        }
    };
#pragma pack(pop)
    
    /**
       interface that handles communication with the db
     */
    class DBConnector {
    public:
        virtual bool call( Message &toSend, Message &response, bool assertOk=true ) = 0;
        virtual void say( Message &toSend ) = 0;
        virtual void sayPiggyBack( Message &toSend ) = 0;
        virtual void checkResponse( const char *data, int nReturned ) {}
    };

    class DBClientCursor : boost::noncopyable {
    public:
        bool more(); // if true, safe to call next()

        /* returns next object in the result cursor.
           on an error at the remote server, you will get back:
             { $err: <string> }
           if you do not want to handle that yourself, call nextSafe().
        */
        BSONObj next();

        BSONObj nextSafe() {
            BSONObj o = next();
            BSONElement e = o.firstElement();
            assert( strcmp(e.fieldName(), "$err") != 0 );
            return o;
        }

        /* cursor no longer valid -- use with tailable cursors.
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

        bool init();

        DBClientCursor( DBConnector *_connector, const char * _ns, BSONObj _query, int _nToReturn,
                        int _nToSkip, BSONObj *_fieldsToReturn, int queryOptions ) :
                connector(_connector),
                ns(_ns),
                query(_query),
                nToReturn(_nToReturn),
                nToSkip(_nToSkip),
                fieldsToReturn(_fieldsToReturn),
                opts(queryOptions),
                m(new Message()) {
            cursorId = 0;
        }

        virtual ~DBClientCursor();

    private:
        DBConnector *connector;
        string ns;
        BSONObj query;
        int nToReturn;
        int nToSkip;
        BSONObj *fieldsToReturn;
        int opts;
        auto_ptr<Message> m;

        long long cursorId;
        int nReturned;
        int pos;
        const char *data;
        void dataReceived();
        void requestMore();
    };


    /**
       the interface that any db connection should implement
     */
    class DBClientInterface : boost::noncopyable {
    public:
        virtual auto_ptr<DBClientCursor> query(const char *ns, BSONObj query, int nToReturn = 0, int nToSkip = 0,
                                               BSONObj *fieldsToReturn = 0, int queryOptions = 0) = 0;

        virtual BSONObj findOne(const char *ns, BSONObj query, BSONObj *fieldsToReturn = 0, int queryOptions = 0) = 0;

        virtual void insert( const char * ns , BSONObj obj ) = 0;

        //virtual void remove( const char * ns , BSONObj obj , bool justOne = 0 ) = 0;

        //virtual void update( const char * ns , BSONObj query , BSONObj obj , bool upsert = 0 ) = 0;
    };

    /**
       db "commands"
       basically just invocations of connection.$cmd.findOne({...});
    */
    class DBClientWithCommands : public DBClientInterface {
        bool isOk(const BSONObj&);
        bool simpleCommand(const char *dbname, BSONObj *info, const char *command);
    public:
        /* Run a database command.  Database commands are represented as BSON objects.  Common database
           commands have prebuilt helper functions -- see below.  If a helper is not available you can
           directly call runCommand.

           dbname - database name.  Use "admin" for global administrative commands.
           cmd    - the command object to execute.  For example, { ismaster : 1 }
           info   - the result object the database returns. Typically has { ok : ..., errmsg : ... } fields
                    set.

           returns: true if the command returned "ok".
        */
        bool runCommand(const char *dbname, BSONObj cmd, BSONObj &info);

        /* returns true in isMaster parm if this db is the current master
           of a replica pair.

           pass in info for more details e.g.:
             { "ismaster" : 1.0 , "msg" : "not paired" , "ok" : 1.0  }

           returns true if command invoked successfully.
        */
        virtual bool isMaster(bool& isMaster, BSONObj *info=0);

        /*
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
        bool createCollection(const char *ns, unsigned size = 0, bool capped = false, int max = 0, BSONObj *info = 0);

        /* Erase / drop an entire database */
        bool dropDatabase(const char *dbname, BSONObj *info = 0) {
            return simpleCommand(dbname, info, "dropDatabase");
        }

        /* Perform a repair and compaction of the specified database.  May take a long time to run.  Disk space
           must be available equal to the size of the database while repairing.
        */
        bool repairDatabase(const char *dbname, BSONObj *info = 0) {
            return simpleCommand(dbname, info, "repairDatabase");
        }

        /* Copy database from one server or name to another server or name.

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
        bool copyDatabase(const char *fromdb, const char *todb, const char *fromhost = "", BSONObj *info = 0);

        /* The Mongo database provides built-in performance profiling capabilities.  Uset setDbProfilingLevel()
           to enable.  Profiling information is then written to the system.profiling collection, which one can
           then query.
        */
        enum ProfilingLevel {
            ProfileOff = 0,
            ProfileSlow = 1, // log very slow (>100ms) operations
            ProfileAll = 2
        };
        bool setDbProfilingLevel(const char *dbname, ProfilingLevel level, BSONObj *info = 0);
        bool getDbProfilingLevel(const char *dbname, ProfilingLevel& level, BSONObj *info = 0);

        /* Run javascript code on the database server.
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
        bool eval(const char *dbname, const char *jscode, BSONObj& info, BSONElement& retValue, BSONObj *args = 0);

        /* The following helpers are simply more convenient forms of eval() for certain common cases */

        /* invocation with no return value of interest -- with or without one simple parameter */
        bool eval(const char *dbname, const char *jscode);
        template< class T >
        bool eval(const char *dbname, const char *jscode, T parm1) {
            BSONObj info;
            BSONElement retValue;
            BSONObjBuilder b;
            b.append("0", parm1);
            BSONObj args = b.done();
            return eval(dbname, jscode, info, retValue, &args);
        }

        /* invocation with one parm to server and one numeric field (either int or double) returned */
        template< class T, class NumType >
        bool eval(const char *dbname, const char *jscode, T parm1, NumType& ret) {
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

        virtual string toString() = 0;
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
        virtual auto_ptr<DBClientCursor> query(const char *ns, BSONObj query, int nToReturn = 0, int nToSkip = 0,
                                               BSONObj *fieldsToReturn = 0, int queryOptions = 0);

        /**
           @return a single object that matches the query.  if none do, then the object is empty
           @throws AssertionException
        */
        virtual BSONObj findOne(const char *ns, BSONObj query, BSONObj *fieldsToReturn = 0, int queryOptions = 0);
        
        /**
           insert an object into the database
         */
        virtual void insert( const char * ns , BSONObj obj );


        /**
           remove matching objects from the database
           @param justOne if this true, then once a single match is found will stop
         */
        virtual void remove( const char * ns , BSONObj obj , bool justOne = 0 );
        
        /**
           updates objects matching query
         */
        virtual void update( const char * ns , BSONObj query , BSONObj obj , bool upsert = 0 );

        /**
           if name isn't specified, it will be created from the keys (reccomended)
           @return whether or not sent message to db.
             should be true on first call, false on subsequent unless resetIndexCache was called
         */
        virtual bool ensureIndex( const char * ns , BSONObj keys , const char * name = 0 );

        /**
           clears the index cache, so the subsequent call to ensureIndex for any index will go to the server
         */
        virtual void resetIndexCache();

    private:
        set<string> _seenIndexes;
    };
    
    class DBClientPaired;
    
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
        void checkConnection();
    public:

        /**
           @param _autoReconnect whether or not to reconnect on a db or socket failure
         */
        DBClientConnection(bool _autoReconnect=false,DBClientPaired* cp=0) :
                clientPaired(cp), failed(false), autoReconnect(_autoReconnect), lastReconnectTry(0) { }


        /**
           If autoReconnect is true, you can try to use the DBClientConnection even when
           false was returned -- it will try to connect again.

           @param serverHostname host to connect to.  can include port number ( 127.0.0.1 , 127.0.0.1:5555 )
           @param errmsg any relevant error message will appneded to the string
           @return false if fails to connect.
        */
        virtual bool connect(const char *serverHostname, string& errmsg);

        virtual auto_ptr<DBClientCursor> query(const char *ns, BSONObj query, int nToReturn = 0, int nToSkip = 0,
                                               BSONObj *fieldsToReturn = 0, int queryOptions = 0) {
            checkConnection();
            return DBClientBase::query( ns, query, nToReturn, nToSkip, fieldsToReturn, queryOptions );
        }

        /**
           @return whether or not this connection is in a failed state
         */
        bool isFailed() const {
            return failed;
        }

        /**
           this gives you access to the low level interface.
           not reccomented to use
         */
        MessagingPort& port() {
            return *p.get();
        }

        string toStringLong() const {
            stringstream ss;
            ss << serverAddress;
            if ( failed ) ss << " failed";
            return ss.str();
        }
        string toString() {
            return serverAddress;
        }

    protected:
        virtual bool call( Message &toSend, Message &response, bool assertOk = true );
        virtual void say( Message &toSend );
        virtual void sayPiggyBack( Message &toSend );
        virtual void checkResponse( const char *data, int nReturned );
    };

    /* Use this class to connect to a replica pair of servers.  The class will manage
       checking for which is master, and do failover automatically.
    */
    class DBClientPaired : public DBClientWithCommands {
        DBClientConnection left,right;
        enum State {
            NotSetL=0,
            NotSetR=1,
            Left, Right
        } master;

        void _checkMaster();
        DBClientConnection& checkMaster();

    public:
        DBClientPaired();

        /* Returns false is neither member of the pair were reachable, or neither is
           master, although,
           when false returned, you can still try to use this connection object, it will
           try reconnects.
           */
        bool connect(const char *serverHostname1, const char *serverHostname2);

        /* throws userassertion "no master found" */
        virtual
        auto_ptr<DBClientCursor> query(const char *ns, BSONObj query, int nToReturn = 0, int nToSkip = 0,
                                       BSONObj *fieldsToReturn = 0, int queryOptions = 0);

        /* throws userassertion "no master found" */
        virtual
        BSONObj findOne(const char *ns, BSONObj query, BSONObj *fieldsToReturn = 0, int queryOptions = 0);

        // Not implemented
        virtual void insert( const char * ns , BSONObj obj ) {
            assert( false );
        }

        string toString();

        /* notification that we got a "not master" error.
         */
        void isntMaster() {
            master = ( ( master == Left ) ? NotSetR : NotSetL );
        }
    };



} // namespace mongo
