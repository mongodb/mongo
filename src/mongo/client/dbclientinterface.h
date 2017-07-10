/**
 *    Copyright (C) 2008-2015 MongoDB Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <cstdint>

#include "mongo/base/string_data.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/index_spec.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/query.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/protocol.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/stdx/functional.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/abstract_message_port.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/op_msg.h"

namespace mongo {

namespace executor {
struct RemoteCommandResponse;
}

class DBClientCursor;
class DBClientCursorBatchIterator;

/**
 * Represents a full query description, including all options required for the query to be passed on
 * to other hosts
 */
class QuerySpec {
    std::string _ns;
    int _ntoskip;
    int _ntoreturn;
    int _options;
    BSONObj _query;
    BSONObj _fields;
    Query _queryObj;

public:
    QuerySpec(const std::string& ns,
              const BSONObj& query,
              const BSONObj& fields,
              int ntoskip,
              int ntoreturn,
              int options)
        : _ns(ns),
          _ntoskip(ntoskip),
          _ntoreturn(ntoreturn),
          _options(options),
          _query(query.getOwned()),
          _fields(fields.getOwned()),
          _queryObj(_query) {}

    QuerySpec() {}

    bool isEmpty() const {
        return _ns.size() == 0;
    }

    bool isExplain() const {
        return _queryObj.isExplain();
    }
    BSONObj filter() const {
        return _queryObj.getFilter();
    }

    BSONObj hint() const {
        return _queryObj.getHint();
    }
    BSONObj sort() const {
        return _queryObj.getSort();
    }
    BSONObj query() const {
        return _query;
    }
    BSONObj fields() const {
        return _fields;
    }
    BSONObj* fieldsData() {
        return &_fields;
    }

    // don't love this, but needed downstrem
    const BSONObj* fieldsPtr() const {
        return &_fields;
    }

    std::string ns() const {
        return _ns;
    }
    int ntoskip() const {
        return _ntoskip;
    }
    int ntoreturn() const {
        return _ntoreturn;
    }
    int options() const {
        return _options;
    }

    void setFields(BSONObj& o) {
        _fields = o.getOwned();
    }

    std::string toString() const {
        return str::stream() << "QSpec "
                             << BSON("ns" << _ns << "n2skip" << _ntoskip << "n2return" << _ntoreturn
                                          << "options"
                                          << _options
                                          << "query"
                                          << _query
                                          << "fields"
                                          << _fields);
    }
};


/** Typically one uses the QUERY(...) macro to construct a Query object.
    Example: QUERY( "age" << 33 << "school" << "UCLA" )
*/
#define QUERY(x) ::mongo::Query(BSON(x))

// Useful utilities for namespaces
/** @return the database name portion of an ns std::string */
std::string nsGetDB(const std::string& ns);

/** @return the collection name portion of an ns std::string */
std::string nsGetCollection(const std::string& ns);

/**
 abstract class that implements the core db operations
 */
class DBClientBase {
    MONGO_DISALLOW_COPYING(DBClientBase);

public:
    DBClientBase()
        : _logLevel(logger::LogSeverity::Log()),
          _connectionId(ConnectionIdSequence.fetchAndAdd(1)),
          _cachedAvailableOptions((enum QueryOptions)0),
          _haveCachedAvailableOptions(false) {}

    virtual ~DBClientBase() {}

    /**
       @return a single object that matches the query.  if none do, then the object is empty
       @throws AssertionException
    */
    virtual BSONObj findOne(const std::string& ns,
                            const Query& query,
                            const BSONObj* fieldsToReturn = 0,
                            int queryOptions = 0);

    /** query N objects from the database into an array.  makes sense mostly when you want a small
     * number of results.  if a huge number, use query() and iterate the cursor.
    */
    void findN(std::vector<BSONObj>& out,
               const std::string& ns,
               Query query,
               int nToReturn,
               int nToSkip = 0,
               const BSONObj* fieldsToReturn = 0,
               int queryOptions = 0);

    virtual std::string getServerAddress() const = 0;

    /** helper function.  run a simple command where the command expression is simply
          { command : 1 }
        @param info -- where to put result object.  may be null if caller doesn't need that info
        @param command -- command name
        @return true if the command returned "ok".
     */
    bool simpleCommand(const std::string& dbname, BSONObj* info, const std::string& command);

    rpc::ProtocolSet getClientRPCProtocols() const;
    rpc::ProtocolSet getServerRPCProtocols() const;

    void setClientRPCProtocols(rpc::ProtocolSet clientProtocols);

    /**
     * actualServer is set to the actual server where they call went if there was a choice (for
     * example SlaveOk).
     */
    virtual bool call(Message& toSend,
                      Message& response,
                      bool assertOk = true,
                      std::string* actualServer = nullptr) = 0;

    virtual void say(Message& toSend,
                     bool isRetry = false,
                     std::string* actualServer = nullptr) = 0;

    /* used by QueryOption_Exhaust.  To use that your subclass must implement this. */
    virtual bool recv(Message& m, int lastRequestId) {
        verify(false);
        return false;
    }

    // In general, for lazy queries, we'll need to say, recv, then checkResponse
    virtual void checkResponse(const std::vector<BSONObj>& batch,
                               bool networkError,
                               bool* retry = nullptr,
                               std::string* targetHost = nullptr) {
        if (retry)
            *retry = false;
        if (targetHost)
            *targetHost = "";
    }

    virtual bool lazySupported() const = 0;

    /**
     * Sets a RequestMetadataWriter on this connection.
     *
     * TODO: support multiple metadata writers.
     */
    virtual void setRequestMetadataWriter(rpc::RequestMetadataWriter writer);

    /**
     * Gets the RequestMetadataWriter that is set on this connection. This may
     * be an uninitialized stdx::function, so it should be checked for validity
     * with operator bool() first.
     */
    const rpc::RequestMetadataWriter& getRequestMetadataWriter();

    /**
     * Sets a ReplyMetadataReader on this connection.
     *
     * TODO: support multiple metadata readers.
     */
    virtual void setReplyMetadataReader(rpc::ReplyMetadataReader reader);

    /**
     * Gets the ReplyMetadataReader that is set on this connection. This may
     * be an uninitialized stdx::function, so it should be checked for validity
     * with operator bool() first.
     */
    const rpc::ReplyMetadataReader& getReplyMetadataReader();

    /**
     * Runs the specified command request.
     */
    virtual std::pair<rpc::UniqueReply, DBClientBase*> runCommandWithTarget(OpMsgRequest request);

    /**
     * Runs the specified command request. This thin wrapper just unwraps the reply and ignores the
     * target connection from the above runCommandWithTarget().
     */
    rpc::UniqueReply runCommand(OpMsgRequest request) {
        return runCommandWithTarget(std::move(request)).first;
    }

    /** Run a database command.  Database commands are represented as BSON objects.  Common database
        commands have prebuilt helper functions -- see below.  If a helper is not available you can
        directly call runCommand.

        @param dbname database name.  Use "admin" for global administrative commands.
        @param cmd  the command object to execute.  For example, { ismaster : 1 }
        @param info the result object the database returns. Typically has { ok : ..., errmsg : ... }
                    fields set.
        @param options see enum QueryOptions - normally not needed to run a command
        @param auth if set, the BSONObj representation will be appended to the command object sent

        @return true if the command returned "ok".
    */
    bool runCommand(const std::string& dbname, BSONObj cmd, BSONObj& info, int options = 0);

    /*
     * This wraps up the runCommand function avove, but returns the DBClient that actually ran
     * the command. When called against a replica set, this will return the specific
     * replica set member the command ran against.
     *
     * This is used in the shell so that cursors can send getMore through the correct connection.
     */
    std::tuple<bool, DBClientBase*> runCommandWithTarget(const std::string& dbname,
                                                         BSONObj cmd,
                                                         BSONObj& info,
                                                         int options = 0);

    /**
    * Authenticates to another cluster member using appropriate authentication data.
    * Uses getInternalUserAuthParams() to retrive authentication parameters.
    * @return true if the authentication was succesful
    */
    bool authenticateInternalUser();

    /**
     * Authenticate a user.
     *
     * The "params" BSONObj should be initialized with some of the fields below.  Which fields
     * are required depends on the mechanism, which is mandatory.
     *
     *     "mechanism": The std::string name of the sasl mechanism to use.  Mandatory.
     *     "user": The std::string name of the user to authenticate.  Mandatory.
     *     "db": The database target of the auth command, which identifies the location
     *         of the credential information for the user.  May be "$external" if
     *         credential information is stored outside of the mongo cluster.  Mandatory.
     *     "pwd": The password data.
     *     "digestPassword": Boolean, set to true if the "pwd" is undigested (default).
     *     "serviceName": The GSSAPI service name to use.  Defaults to "mongodb".
     *     "serviceHostname": The GSSAPI hostname to use.  Defaults to the name of the remote
     *          host.
     *
     * Other fields in "params" are silently ignored.
     *
     * Returns normally on success, and throws on error.  Throws a DBException with getCode() ==
     * ErrorCodes::AuthenticationFailed if authentication is rejected.  All other exceptions are
     * tantamount to authentication failure, but may also indicate more serious problems.
     */
    void auth(const BSONObj& params);

    /** Authorize access to a particular database.
        Authentication is separate for each database on the server -- you may authenticate for any
        number of databases on a single connection.
        The "admin" database is special and once authenticated provides access to all databases on
        the server.
        @param      digestPassword  if password is plain text, set this to true.  otherwise assumed
                                    to be pre-digested
        @param[out] authLevel       level of authentication for the given user
        @return true if successful
    */
    bool auth(const std::string& dbname,
              const std::string& username,
              const std::string& pwd,
              std::string& errmsg,
              bool digestPassword = true);

    /**
     * Logs out the connection for the given database.
     *
     * @param dbname the database to logout from.
     * @param info the result object for the logout command (provided for backwards
     *     compatibility with mongo shell)
     */
    virtual void logout(const std::string& dbname, BSONObj& info);

    /** count number of objects in collection ns that match the query criteria specified
        throws UserAssertion if database returns an error
    */
    virtual unsigned long long count(const std::string& ns,
                                     const BSONObj& query = BSONObj(),
                                     int options = 0,
                                     int limit = 0,
                                     int skip = 0);

    static std::string createPasswordDigest(const std::string& username,
                                            const std::string& clearTextPassword);

    /** returns true in isMaster parm if this db is the current master
       of a replica pair.

       pass in info for more details e.g.:
         { "ismaster" : 1.0 , "msg" : "not paired" , "ok" : 1.0  }

       returns true if command invoked successfully.
    */
    virtual bool isMaster(bool& isMaster, BSONObj* info = 0);

    /**
       Create a new collection in the database.  Normally, collection creation is automatic.  You
       would use this function if you wish to specify special options on creation.

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
    bool createCollection(const std::string& ns,
                          long long size = 0,
                          bool capped = false,
                          int max = 0,
                          BSONObj* info = 0);

    /** Get error result from the last write operation (insert/update/delete) on this connection.
        db doesn't change the command's behavior - it is just for auth checks.
        @return error message text, or empty std::string if no error.
    */
    std::string getLastError(
        const std::string& db, bool fsync = false, bool j = false, int w = 0, int wtimeout = 0);
    /**
     * Same as the form of getLastError that takes a dbname, but just uses the admin DB.
     */
    std::string getLastError(bool fsync = false, bool j = false, int w = 0, int wtimeout = 0);

    /** Get error result from the last write operation (insert/update/delete) on this connection.
        db doesn't change the command's behavior - it is just for auth checks.
        @return full error object.

        If "w" is -1, wait for propagation to majority of nodes.
        If "wtimeout" is 0, the operation will block indefinitely if needed.
    */
    virtual BSONObj getLastErrorDetailed(
        const std::string& db, bool fsync = false, bool j = false, int w = 0, int wtimeout = 0);
    /**
     * Same as the form of getLastErrorDetailed that takes a dbname, but just uses the admin DB.
     */
    virtual BSONObj getLastErrorDetailed(bool fsync = false,
                                         bool j = false,
                                         int w = 0,
                                         int wtimeout = 0);

    /** Can be called with the returned value from getLastErrorDetailed to extract an error string.
        If all you need is the string, just call getLastError() instead.
    */
    static std::string getLastErrorString(const BSONObj& res);

    /** Return the last error which has occurred, even if not the very last operation.

       @return { err : <error message>, nPrev : <how_many_ops_back_occurred>, ok : 1 }

       result.err will be null if no error has occurred.
    */
    BSONObj getPrevError();

    /** Delete the specified collection.
     *  @param info An optional output parameter that receives the result object the database
     *  returns from the drop command.  May be null if the caller doesn't need that info.
     */
    virtual bool dropCollection(const std::string& ns,
                                const WriteConcernOptions& writeConcern = WriteConcernOptions(),
                                BSONObj* info = nullptr) {
        std::string db = nsGetDB(ns);
        std::string coll = nsGetCollection(ns);
        uassert(10011, "no collection name", coll.size());

        BSONObj temp;
        if (info == nullptr) {
            info = &temp;
        }

        bool res = runCommand(
            db.c_str(), BSON("drop" << coll << "writeConcern" << writeConcern.toBSON()), *info);
        return res;
    }

    /** Copy database from one server or name to another server or name.

       Generally, you should dropDatabase() first as otherwise the copied information will MERGE
       into whatever data is already present in this database.

       For security reasons this function only works when you are authorized to access the "admin"
       db.  However, if you have access to said db, you can copy any database from one place to
       another.
       TODO: this needs enhancement to be more flexible in terms of security.

       This method provides a way to "rename" a database by copying it to a new db name and
       location.  The copy is "repaired" and compacted.

       fromdb   database name from which to copy.
       todb     database name to copy to.
       fromhost hostname of the database (and optionally, ":port") from which to
                copy the data.  copies from self if "".

       returns true if successful
    */
    bool copyDatabase(const std::string& fromdb,
                      const std::string& todb,
                      const std::string& fromhost = "",
                      BSONObj* info = 0);

    /** Run javascript code on the database server.
       dbname    database SavedContext in which the code runs. The javascript variable 'db' will be
                 assigned to this database when the function is invoked.
       jscode    source code for a javascript function.
       info      the command object which contains any information on the invocation result
                 including the return value and other information.  If an error occurs running the
                 jscode, error information will be in info.  (try "log() << info.toString()")
       retValue  return value from the jscode function.
       args      args to pass to the jscode function.  when invoked, the 'args' variable will be
                 defined for use by the jscode.

       returns true if runs ok.

       See testDbEval() in dbclient.cpp for an example of usage.
    */
    bool eval(const std::string& dbname,
              const std::string& jscode,
              BSONObj& info,
              BSONElement& retValue,
              BSONObj* args = 0);

    /** validate a collection, checking for errors and reporting back statistics.
        this operation is slow and blocking.
     */
    bool validate(const std::string& ns, bool scandata = true) {
        BSONObj cmd = BSON("validate" << nsGetCollection(ns) << "scandata" << scandata);
        BSONObj info;
        return runCommand(nsGetDB(ns).c_str(), cmd, info);
    }

    /* The following helpers are simply more convenient forms of eval() for certain common cases */

    /* invocation with no return value of interest -- with or without one simple parameter */
    bool eval(const std::string& dbname, const std::string& jscode);
    template <class T>
    bool eval(const std::string& dbname, const std::string& jscode, T parm1) {
        BSONObj info;
        BSONElement retValue;
        BSONObjBuilder b;
        b.append("0", parm1);
        BSONObj args = b.done();
        return eval(dbname, jscode, info, retValue, &args);
    }

    /** eval invocation with one parm to server and one numeric field (either int or double)
     * returned */
    template <class T, class NumType>
    bool eval(const std::string& dbname, const std::string& jscode, T parm1, NumType& ret) {
        BSONObj info;
        BSONElement retValue;
        BSONObjBuilder b;
        b.append("0", parm1);
        BSONObj args = b.done();
        if (!eval(dbname, jscode, info, retValue, &args))
            return false;
        ret = (NumType)retValue.number();
        return true;
    }

    /**
     * { name : "<short collection name>",
     *   options : { }
     * }
     */
    std::list<BSONObj> getCollectionInfos(const std::string& db, const BSONObj& filter = BSONObj());

    bool exists(const std::string& ns);

    /** Create an index on the collection 'ns' as described by the given keys. If you wish
     *  to specify options, see the more flexible overload of 'createIndex' which takes an
     *  IndexSpec object. Failure to construct the index is reported by throwing a
     *  UserException.
     *
     *  @param ns Namespace on which to create the index
     *  @param keys Document describing keys and index types. You must provide at least one
     *  field and its direction.
     */
    void createIndex(StringData ns, const BSONObj& keys) {
        return createIndex(ns, IndexSpec().addKeys(keys));
    }

    /** Create an index on the collection 'ns' as described by the given
     *  descriptor. Failure to construct the index is reported by throwing a
     *  UserException.
     *
     *  @param ns Namespace on which to create the index
     *  @param descriptor Configuration object describing the index to create. The
     *  descriptor must describe at least one key and index type.
     */
    virtual void createIndex(StringData ns, const IndexSpec& descriptor);

    virtual std::list<BSONObj> getIndexSpecs(const std::string& ns, int options = 0);

    virtual void dropIndex(const std::string& ns, BSONObj keys);
    virtual void dropIndex(const std::string& ns, const std::string& indexName);

    /**
       drops all indexes for the collection
     */
    virtual void dropIndexes(const std::string& ns);

    virtual void reIndex(const std::string& ns);

    static std::string genIndexName(const BSONObj& keys);

    /** Erase / drop an entire database */
    virtual bool dropDatabase(const std::string& dbname,
                              const WriteConcernOptions& writeConcern = WriteConcernOptions(),
                              BSONObj* info = nullptr) {
        BSONObj o;
        if (info == nullptr)
            info = &o;
        return runCommand(
            dbname, BSON("dropDatabase" << 1 << "writeConcern" << writeConcern.toBSON()), *info);
    }

    virtual std::string toString() const = 0;

    /**
     * Run a pseudo-command such as sys.inprog/currentOp, sys.killop/killOp
     * or sys.unlock/fsyncUnlock
     *
     * The real command will be tried first, and if the remote server does not
     * implement the command, it will fall back to the pseudoCommand.
     *
     * The cmdArgs parameter should NOT include {<commandName>: 1}.
     *
     * TODO: remove after MongoDB 3.2 is released and replace all callers with
     * a call to plain runCommand
     */
    virtual bool runPseudoCommand(StringData db,
                                  StringData realCommandName,
                                  StringData pseudoCommandCol,
                                  const BSONObj& cmdArgs,
                                  BSONObj& info,
                                  int options = 0);

    /**
     * Reconnect if needed and allowed.
     */
    virtual void checkConnection() {}

    static const uint64_t INVALID_SOCK_CREATION_TIME;

    long long getConnectionId() const {
        return _connectionId;
    }

    virtual int getMinWireVersion() = 0;
    virtual int getMaxWireVersion() = 0;

    /** send a query to the database.
     @param ns namespace to query, format is <dbname>.<collectname>[.<collectname>]*
     @param query query to perform on the collection.  this is a BSONObj (binary JSON)
     You may format as
       { query: { ... }, orderby: { ... } }
     to specify a sort order.
     @param nToReturn n to return (i.e., limit).  0 = unlimited
     @param nToSkip start with the nth item
     @param fieldsToReturn optional template of which fields to select. if unspecified,
            returns all fields
     @param queryOptions see options enum at top of this file

     @return    cursor.   0 if error (connection failure)
     @throws AssertionException
    */
    virtual std::unique_ptr<DBClientCursor> query(const std::string& ns,
                                                  Query query,
                                                  int nToReturn = 0,
                                                  int nToSkip = 0,
                                                  const BSONObj* fieldsToReturn = 0,
                                                  int queryOptions = 0,
                                                  int batchSize = 0);


    /** Uses QueryOption_Exhaust, when available.

        Exhaust mode sends back all data queries as fast as possible, with no back-and-forth for
        OP_GETMORE.  If you are certain you will exhaust the query, it could be useful.

        Use the DBClientCursorBatchIterator version, below, if you want to do items in large
        blocks, perhaps to avoid granular locking and such.
     */
    virtual unsigned long long query(stdx::function<void(const BSONObj&)> f,
                                     const std::string& ns,
                                     Query query,
                                     const BSONObj* fieldsToReturn = 0,
                                     int queryOptions = 0);

    virtual unsigned long long query(stdx::function<void(DBClientCursorBatchIterator&)> f,
                                     const std::string& ns,
                                     Query query,
                                     const BSONObj* fieldsToReturn = 0,
                                     int queryOptions = 0);


    /** don't use this - called automatically by DBClientCursor for you
        @param cursorId id of cursor to retrieve
        @return an handle to a previously allocated cursor
        @throws AssertionException
     */
    virtual std::unique_ptr<DBClientCursor> getMore(const std::string& ns,
                                                    long long cursorId,
                                                    int nToReturn = 0,
                                                    int options = 0);

    /**
       insert an object into the database
     */
    virtual void insert(const std::string& ns, BSONObj obj, int flags = 0);

    /**
       insert a vector of objects into the database
     */
    virtual void insert(const std::string& ns, const std::vector<BSONObj>& v, int flags = 0);

    /**
       updates objects matching query
     */
    virtual void update(
        const std::string& ns, Query query, BSONObj obj, bool upsert = false, bool multi = false);

    virtual void update(const std::string& ns, Query query, BSONObj obj, int flags);

    virtual void remove(const std::string& ns, Query query, int flags = 0);

    virtual bool isFailed() const = 0;

    /**
     * if not checked recently, checks whether the underlying socket/sockets are still valid
     */
    virtual bool isStillConnected() = 0;

    virtual void killCursor(const NamespaceString& ns, long long cursorID);

    virtual ConnectionString::ConnectionType type() const = 0;

    virtual double getSoTimeout() const = 0;

    virtual uint64_t getSockCreationMicroSec() const {
        return INVALID_SOCK_CREATION_TIME;
    }

    virtual void reset() {}

protected:
    /** if the result of a command is ok*/
    bool isOk(const BSONObj&);

    /** if the element contains a not master error */
    bool isNotMasterErrorString(const BSONElement& e);

    BSONObj _countCmd(
        const std::string& ns, const BSONObj& query, int options, int limit, int skip);

    /**
     * Look up the options available on this client.  Caches the answer from
     * _lookupAvailableOptions(), below.
     */
    QueryOptions availableOptions();

    virtual QueryOptions _lookupAvailableOptions();

    virtual void _auth(const BSONObj& params);

    // should be set by subclasses during connection.
    void _setServerRPCProtocols(rpc::ProtocolSet serverProtocols);

    /** controls how chatty the client is about network errors & such.  See log.h */
    const logger::LogSeverity _logLevel;

    static AtomicInt64 ConnectionIdSequence;
    long long _connectionId;  // unique connection id for this connection

private:
    /**
     * The rpc protocols this client supports.
     *
     */
    rpc::ProtocolSet _clientRPCProtocols{rpc::supports::kAll};

    /**
     * The rpc protocol the remote server(s) support. We support 'opQueryOnly' by default unless
     * we detect support for OP_COMMAND at connection time.
     */
    rpc::ProtocolSet _serverRPCProtocols{rpc::supports::kOpQueryOnly};

    rpc::RequestMetadataWriter _metadataWriter;
    rpc::ReplyMetadataReader _metadataReader;

    enum QueryOptions _cachedAvailableOptions;
    bool _haveCachedAvailableOptions;
};  // DBClientBase

/**
    A basic connection to the database.
    This is the main entry point for talking to a simple Mongo setup
*/
class DBClientConnection : public DBClientBase {
public:
    using DBClientBase::query;

    /**
     * A hook used to validate the reply of an 'isMaster' command during connection. If the hook
     * returns a non-OK Status, the DBClientConnection object will disconnect from the remote
     * server. This function must not throw - it can only indicate failure by returning a non-OK
     * status.
     */
    using HandshakeValidationHook =
        stdx::function<Status(const executor::RemoteCommandResponse& isMasterReply)>;

    /**
       @param _autoReconnect if true, automatically reconnect on a connection failure
       @param timeout tcp timeout in seconds - this is for read/write, not connect.
       Connect timeout is fixed, but short, at 5 seconds.
     */
    DBClientConnection(bool _autoReconnect = false,
                       double so_timeout = 0,
                       MongoURI uri = {},
                       const HandshakeValidationHook& hook = HandshakeValidationHook());

    virtual ~DBClientConnection() {
        _numConnections.fetchAndAdd(-1);
    }

    /**
     * Connect to a Mongo database server.
     *
     * If autoReconnect is true, you can try to use the DBClientConnection even when
     * false was returned -- it will try to connect again.
     *
     * @param server server to connect to.
     * @param errmsg any relevant error message will appended to the string
     * @return false if fails to connect.
     */
    virtual bool connect(const HostAndPort& server,
                         StringData applicationName,
                         std::string& errmsg);

    /**
     * Semantically equivalent to the previous connect method, but returns a Status
     * instead of taking an errmsg out parameter. Also allows optional validation of the reply to
     * the 'isMaster' command executed during connection.
     *
     * @param server The server to connect to.
     * @param a hook to validate the 'isMaster' reply received during connection. If the hook
     * fails, the connection will be terminated and a non-OK status will be returned.
     */
    Status connect(const HostAndPort& server, StringData applicationName);

    /**
     * This version of connect does not run 'isMaster' after creating a TCP connection to the
     * remote host. This method should be used only when calling 'isMaster' would create a deadlock,
     * such as in 'isSelf'.
     *
     * @param server The server to connect to.
     */
    Status connectSocketOnly(const HostAndPort& server);

    /** Connect to a Mongo database server.  Exception throwing version.
        Throws a UserException if cannot connect.

       If autoReconnect is true, you can try to use the DBClientConnection even when
       false was returned -- it will try to connect again.

       @param serverHostname host to connect to.  can include port number ( 127.0.0.1 ,
                               127.0.0.1:5555 )
    */

    /**
     * Logs out the connection for the given database.
     *
     * @param dbname the database to logout from.
     * @param info the result object for the logout command (provided for backwards
     *     compatibility with mongo shell)
     */
    virtual void logout(const std::string& dbname, BSONObj& info);

    virtual std::unique_ptr<DBClientCursor> query(const std::string& ns,
                                                  Query query = Query(),
                                                  int nToReturn = 0,
                                                  int nToSkip = 0,
                                                  const BSONObj* fieldsToReturn = 0,
                                                  int queryOptions = 0,
                                                  int batchSize = 0) {
        checkConnection();
        return DBClientBase::query(
            ns, query, nToReturn, nToSkip, fieldsToReturn, queryOptions, batchSize);
    }

    virtual unsigned long long query(stdx::function<void(DBClientCursorBatchIterator&)> f,
                                     const std::string& ns,
                                     Query query,
                                     const BSONObj* fieldsToReturn,
                                     int queryOptions);

    using DBClientBase::runCommandWithTarget;
    std::pair<rpc::UniqueReply, DBClientBase*> runCommandWithTarget(OpMsgRequest request) override;

    /**
       @return true if this connection is currently in a failed state.  When autoreconnect is on,
               a connection will transition back to an ok state after reconnecting.
     */
    bool isFailed() const {
        return _failed;
    }

    bool isStillConnected() {
        return _port ? _port->isStillConnected() : true;
    }

    void setWireVersions(int minWireVersion, int maxWireVersion) {
        _minWireVersion = minWireVersion;
        _maxWireVersion = maxWireVersion;
    }

    int getMinWireVersion() final {
        return _minWireVersion;
    }

    int getMaxWireVersion() final {
        return _maxWireVersion;
    }

    AbstractMessagingPort& port() {
        verify(_port);
        return *_port;
    }

    std::string toString() const {
        std::stringstream ss;
        ss << _serverAddress;
        if (!_resolvedAddress.empty())
            ss << " (" << _resolvedAddress << ")";
        if (_failed)
            ss << " failed";
        return ss.str();
    }

    std::string getServerAddress() const {
        return _serverAddress.toString();
    }
    const HostAndPort& getServerHostAndPort() const {
        return _serverAddress;
    }

    virtual void say(Message& toSend, bool isRetry = false, std::string* actualServer = 0);
    virtual bool recv(Message& m, int lastRequestId);
    virtual void checkResponse(const std::vector<BSONObj>& batch,
                               bool networkError,
                               bool* retry = NULL,
                               std::string* host = NULL);
    virtual bool call(Message& toSend, Message& response, bool assertOk, std::string* actualServer);
    virtual ConnectionString::ConnectionType type() const {
        return ConnectionString::MASTER;
    }
    void setSoTimeout(double timeout);
    double getSoTimeout() const {
        return _so_timeout;
    }

    virtual bool lazySupported() const {
        return true;
    }

    static int getNumConnections() {
        return _numConnections.load();
    }

    /**
     * Set the name of the replica set that this connection is associated to.
     * Note: There is no validation on replSetName.
     */
    void setParentReplSetName(const std::string& replSetName);

    uint64_t getSockCreationMicroSec() const;

    MessageCompressorManager& getCompressorManager() {
        return _compressorManager;
    }

    // throws SocketException if in failed state and not reconnecting or if waiting to reconnect
    void checkConnection() override {
        if (_failed)
            _checkConnection();
    }

protected:
    int _minWireVersion{0};
    int _maxWireVersion{0};

    virtual void _auth(const BSONObj& params);

    std::unique_ptr<AbstractMessagingPort> _port;

    bool _failed;
    const bool autoReconnect;
    Backoff autoReconnectBackoff;

    HostAndPort _serverAddress;
    std::string _resolvedAddress;
    std::string _applicationName;

    void _checkConnection();

    std::map<std::string, BSONObj> authCache;
    double _so_timeout;

    static AtomicInt32 _numConnections;

private:
    /**
     * Checks the BSONElement for the 'not master' keyword and if it does exist,
     * try to inform the replica set monitor that the host this connects to is
     * no longer primary.
     */
    void handleNotMasterResponse(const BSONElement& elemToCheck);

    // Contains the string for the replica set name of the host this is connected to.
    // Should be empty if this connection is not pointing to a replica set member.
    std::string _parentReplSetName;

    // Hook ran on every call to connect()
    HandshakeValidationHook _hook;

    MessageCompressorManager _compressorManager;

    MongoURI _uri;
};

BSONElement getErrField(const BSONObj& result);
bool hasErrField(const BSONObj& result);

inline std::ostream& operator<<(std::ostream& s, const Query& q) {
    return s << q.toString();
}

}  // namespace mongo

#include "mongo/client/dbclientcursor.h"
