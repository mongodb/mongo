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
#include "mongo/client/query.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/protocol.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/abstract_message_port.h"
#include "mongo/util/net/message.h"

namespace mongo {

namespace executor {
struct RemoteCommandResponse;
}

/** the query field 'options' can have these bits set: */
enum QueryOptions {
    /** Tailable means cursor is not closed when the last data is retrieved.  rather, the cursor
     * marks the final object's position.  you can resume using the cursor later, from where it was
       located, if more data were received.  Set on dbQuery and dbGetMore.

       like any "latent cursor", the cursor may become invalid at some point -- for example if that
       final object it references were deleted.  Thus, you should be prepared to requery if you get
       back ResultFlag_CursorNotFound.
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

    /** The server normally times out idle cursors after an inactivity period to prevent excess
     * memory uses
        Set this option to prevent that.
    */
    QueryOption_NoCursorTimeout = 1 << 4,

    /** Use with QueryOption_CursorTailable.  If we are at the end of the data, block for a while
     * rather than returning no data. After a timeout period, we do return as normal.
    */
    QueryOption_AwaitData = 1 << 5,

    /** Stream the data down full blast in multiple "more" packages, on the assumption that the
     * client will fully read all data queried.  Faster when you are pulling a lot of data and know
     * you want to pull it all down.  Note: it is not allowed to not read all the data unless you
     * close the connection.

        Use the query( stdx::function<void(const BSONObj&)> f, ... ) version of the connection's
        query()
        method, and it will take care of all the details for you.
    */
    QueryOption_Exhaust = 1 << 6,

    /** When sharded, this means its ok to return partial results
        Usually we will fail a query if all required shards aren't up
        If this is set, it'll be a partial result set
     */
    QueryOption_PartialResults = 1 << 7,

    QueryOption_AllSupported = QueryOption_CursorTailable | QueryOption_SlaveOk |
        QueryOption_OplogReplay | QueryOption_NoCursorTimeout | QueryOption_AwaitData |
        QueryOption_Exhaust | QueryOption_PartialResults,

    QueryOption_AllSupportedForSharding = QueryOption_CursorTailable | QueryOption_SlaveOk |
        QueryOption_OplogReplay | QueryOption_NoCursorTimeout | QueryOption_AwaitData |
        QueryOption_PartialResults,
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

enum InsertOptions {
    /** With muli-insert keep processing inserts if one fails */
    InsertOption_ContinueOnError = 1 << 0
};

//
// For legacy reasons, the reserved field pre-namespace of certain types of messages is used
// to store options as opposed to the flags after the namespace.  This should be transparent to
// the api user, but we need these constants to disassemble/reassemble the messages correctly.
//

enum ReservedOptions {
    Reserved_InsertOption_ContinueOnError = 1 << 0,
};

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
   The interface that any db connection should implement
 */
class DBClientInterface {
    MONGO_DISALLOW_COPYING(DBClientInterface);

public:
    virtual std::unique_ptr<DBClientCursor> query(const std::string& ns,
                                                  Query query,
                                                  int nToReturn = 0,
                                                  int nToSkip = 0,
                                                  const BSONObj* fieldsToReturn = 0,
                                                  int queryOptions = 0,
                                                  int batchSize = 0) = 0;

    virtual void insert(const std::string& ns, BSONObj obj, int flags = 0) = 0;

    virtual void insert(const std::string& ns, const std::vector<BSONObj>& v, int flags = 0) = 0;

    virtual void remove(const std::string& ns, Query query, int flags) = 0;

    virtual void update(const std::string& ns,
                        Query query,
                        BSONObj obj,
                        bool upsert = false,
                        bool multi = false) = 0;

    virtual void update(const std::string& ns, Query query, BSONObj obj, int flags) = 0;

    virtual ~DBClientInterface() {}

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

    /** don't use this - called automatically by DBClientCursor for you */
    virtual std::unique_ptr<DBClientCursor> getMore(const std::string& ns,
                                                    long long cursorId,
                                                    int nToReturn = 0,
                                                    int options = 0) = 0;

protected:
    DBClientInterface() = default;
};

/**
   DB "commands"
   Basically just invocations of connection.$cmd.findOne({...});
*/
class DBClientWithCommands : public DBClientInterface {
public:
    /** controls how chatty the client is about network errors & such.  See log.h */
    logger::LogSeverity _logLevel;

    DBClientWithCommands()
        : _logLevel(logger::LogSeverity::Log()),
          _cachedAvailableOptions((enum QueryOptions)0),
          _haveCachedAvailableOptions(false) {}

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
    virtual bool recv(Message& m) {
        verify(false);
        return false;
    }

    // In general, for lazy queries, we'll need to say, recv, then checkResponse
    virtual void checkResponse(const char* data,
                               int nReturned,
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
     * Runs a database command. This variant allows the caller to manually specify the metadata
     * for the request, and receive it for the reply.
     *
     * TODO: rename this to runCommand, and change the old one to runCommandLegacy.
     */
    virtual rpc::UniqueReply runCommandWithMetadata(StringData database,
                                                    StringData command,
                                                    const BSONObj& metadata,
                                                    const BSONObj& commandArgs);

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
    virtual bool runCommand(const std::string& dbname,
                            const BSONObj& cmd,
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
       get a list of all the current databases
       uses the { listDatabases : 1 } command.
       throws on error
     */
    std::list<std::string> getDatabaseNames();

    /**
     * Get a list of all the current collections in db.
     * Returns fully qualified names.
     */
    std::list<std::string> getCollectionNames(const std::string& db);

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
};

/**
 abstract class that implements the core db operations
 */
class DBClientBase : public DBClientWithCommands {
protected:
    static AtomicInt64 ConnectionIdSequence;
    long long _connectionId;  // unique connection id for this connection

public:
    static const uint64_t INVALID_SOCK_CREATION_TIME;

    DBClientBase() {
        _connectionId = ConnectionIdSequence.fetchAndAdd(1);
    }

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

    virtual void killCursor(long long cursorID);

    virtual ConnectionString::ConnectionType type() const = 0;

    virtual double getSoTimeout() const = 0;

    virtual uint64_t getSockCreationMicroSec() const {
        return INVALID_SOCK_CREATION_TIME;
    }

    virtual void reset() {}

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
    virtual bool connect(const HostAndPort& server, std::string& errmsg);

    /**
     * Semantically equivalent to the previous connect method, but returns a Status
     * instead of taking an errmsg out parameter. Also allows optional validation of the reply to
     * the 'isMaster' command executed during connection.
     *
     * @param server The server to connect to.
     * @param a hook to validate the 'isMaster' reply received during connection. If the hook
     * fails, the connection will be terminated and a non-OK status will be returned.
     */
    Status connect(const HostAndPort& server);

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

    virtual bool runCommand(const std::string& dbname,
                            const BSONObj& cmd,
                            BSONObj& info,
                            int options = 0);

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
    virtual bool recv(Message& m);
    virtual void checkResponse(const char* data,
                               int nReturned,
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

    void _checkConnection();

    // throws SocketException if in failed state and not reconnecting or if waiting to reconnect
    void checkConnection() {
        if (_failed)
            _checkConnection();
    }

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
};

BSONElement getErrField(const BSONObj& result);
bool hasErrField(const BSONObj& result);

inline std::ostream& operator<<(std::ostream& s, const Query& q) {
    return s << q.toString();
}

}  // namespace mongo

#include "mongo/client/dbclientcursor.h"
