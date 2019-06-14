/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <cstdint>
#include <functional>

#include "mongo/base/string_data.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/index_spec.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/query.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logger/log_severity.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/protocol.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/str.h"

namespace mongo {

namespace executor {
struct RemoteCommandResponse;
}

// Useful utilities for namespaces
/** @return the database name portion of an ns std::string */
std::string nsGetDB(const std::string& ns);

/** @return the collection name portion of an ns std::string */
std::string nsGetCollection(const std::string& ns);

/**
 * This class pre-declares all the "query()" methods for DBClient so the subclasses can mark
 * them as "final" or "override" as appropriate.
 */
class DBClientQueryInterface {
    virtual std::unique_ptr<DBClientCursor> query(const NamespaceStringOrUUID& nsOrUuid,
                                                  Query query,
                                                  int nToReturn = 0,
                                                  int nToSkip = 0,
                                                  const BSONObj* fieldsToReturn = nullptr,
                                                  int queryOptions = 0,
                                                  int batchSize = 0) = 0;

    virtual unsigned long long query(std::function<void(const BSONObj&)> f,
                                     const NamespaceStringOrUUID& nsOrUuid,
                                     Query query,
                                     const BSONObj* fieldsToReturn = nullptr,
                                     int queryOptions = 0,
                                     int batchSize = 0) = 0;

    virtual unsigned long long query(std::function<void(DBClientCursorBatchIterator&)> f,
                                     const NamespaceStringOrUUID& nsOrUuid,
                                     Query query,
                                     const BSONObj* fieldsToReturn = nullptr,
                                     int queryOptions = 0,
                                     int batchSize = 0) = 0;
};

/**
 abstract class that implements the core db operations
 */
class DBClientBase : public DBClientQueryInterface {
    DBClientBase(const DBClientBase&) = delete;
    DBClientBase& operator=(const DBClientBase&) = delete;

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
                            const BSONObj* fieldsToReturn = nullptr,
                            int queryOptions = 0);

    /** query N objects from the database into an array.  makes sense mostly when you want a small
     * number of results.  if a huge number, use query() and iterate the cursor.
    */
    void findN(std::vector<BSONObj>& out,
               const std::string& ns,
               Query query,
               int nToReturn,
               int nToSkip = 0,
               const BSONObj* fieldsToReturn = nullptr,
               int queryOptions = 0);

    /**
     * @return a pair with a single object that matches the filter within the collection specified
     * by the UUID and the namespace of that collection on the queried node.
     *
     * If the command fails, an assertion error is thrown. Otherwise, if no document matches
     * the query, an empty BSONObj is returned.
     * @throws AssertionException
     */
    virtual std::pair<BSONObj, NamespaceString> findOneByUUID(const std::string& db,
                                                              UUID uuid,
                                                              const BSONObj& filter);

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
     * be an uninitialized std::function, so it should be checked for validity
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
     * be an uninitialized std::function, so it should be checked for validity
     * with operator bool() first.
     */
    const rpc::ReplyMetadataReader& getReplyMetadataReader();

    /**
     * Runs the specified command request.
     */
    virtual std::pair<rpc::UniqueReply, DBClientBase*> runCommandWithTarget(OpMsgRequest request);

    /**
     * This shared_ptr overload is used to possibly return a shared_ptr to the replica set member
     * that the command was dispatched to.  It's needed if the caller needs a lifetime for that
     * connection that extends beyond the lifetime, or subsequent calls, against the top level
     * client.
     *
     * It has this slightly insane api because:
     * + we don't want to thread enable_shared_from_this pervasively through the dbclient tree
     * + we use this from places we don't want to know about dbclient_rs (and so don't know if we'll
     *   get our own ptr back).
     * + the only caller who needs this is the shell (because other callers have more control over
     *   lifetime).
     */
    virtual std::pair<rpc::UniqueReply, std::shared_ptr<DBClientBase>> runCommandWithTarget(
        OpMsgRequest request, std::shared_ptr<DBClientBase> me);

    /**
     * Runs the specified command request. This thin wrapper just unwraps the reply and ignores the
     * target connection from the above runCommandWithTarget().
     */
    rpc::UniqueReply runCommand(OpMsgRequest request) {
        return runCommandWithTarget(std::move(request)).first;
    }

    /**
     * Runs the specified command request in fire-and-forget mode and returns the connection that
     * the command was actually sent on. If the connection doesn't support OP_MSG, the request will
     * be run as a normal two-way command and the reply will be ignored after parsing.
     */
    virtual DBClientBase* runFireAndForgetCommand(OpMsgRequest request);

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
     * See the opMsg overload comment for why this function takes a shared_ptr ostensibly to this.
     */
    std::tuple<bool, std::shared_ptr<DBClientBase>> runCommandWithTarget(
        const std::string& dbname,
        BSONObj cmd,
        BSONObj& info,
        std::shared_ptr<DBClientBase> me,
        int options = 0);

    /**
    * Authenticates to another cluster member using appropriate authentication data.
    * @return true if the authentication was successful
    */
    virtual Status authenticateInternalUser();

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
    virtual bool isMaster(bool& isMaster, BSONObj* info = nullptr);

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
                          BSONObj* info = nullptr);

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

    /** validate a collection, checking for errors and reporting back statistics.
        this operation is slow and blocking.
     */
    bool validate(const std::string& ns) {
        BSONObj cmd = BSON("validate" << nsGetCollection(ns));
        BSONObj info;
        return runCommand(nsGetDB(ns).c_str(), cmd, info);
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
     *  AssertionException.
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
     *  AssertionException.
     *
     *  @param ns Namespace on which to create the index
     *  @param descriptor Configuration object describing the index to create. The
     *  descriptor must describe at least one key and index type.
     */
    virtual void createIndex(StringData ns, const IndexSpec& descriptor) {
        std::vector<const IndexSpec*> toBuild;
        toBuild.push_back(&descriptor);
        createIndexes(ns, toBuild);
    }

    virtual void createIndexes(StringData ns, const std::vector<const IndexSpec*>& descriptor);

    /**
     * Creates indexes on the collection 'ns' as described by 'specs'.
     *
     * Failure to construct the indexes is reported by throwing an AssertionException.
     */
    virtual void createIndexes(StringData ns, const std::vector<BSONObj>& specs);

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

    const std::vector<std::string>& getIsMasterSaslMechanisms() const {
        return _saslMechsForAuth;
    }

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
    std::unique_ptr<DBClientCursor> query(const NamespaceStringOrUUID& nsOrUuid,
                                          Query query,
                                          int nToReturn = 0,
                                          int nToSkip = 0,
                                          const BSONObj* fieldsToReturn = nullptr,
                                          int queryOptions = 0,
                                          int batchSize = 0) override;


    /** Uses QueryOption_Exhaust, when available and specified in 'queryOptions'.

        Exhaust mode sends back all data queries as fast as possible, with no back-and-forth for
        OP_GETMORE.  If you are certain you will exhaust the query, it could be useful.  If
        exhaust mode is not specified in 'queryOptions' or not available, this call transparently
        falls back to using ordinary getMores.

        Use the DBClientCursorBatchIterator version, below, if you want to do items in large
        blocks, perhaps to avoid granular locking and such.

        Note:
        The version that takes a BSONObj cannot return the namespace queried when the query is
        is done by UUID.  If this is required, use the DBClientBatchIterator version.
     */
    unsigned long long query(std::function<void(const BSONObj&)> f,
                             const NamespaceStringOrUUID& nsOrUuid,
                             Query query,
                             const BSONObj* fieldsToReturn = nullptr,
                             int queryOptions = QueryOption_Exhaust,
                             int batchSize = 0) final;

    unsigned long long query(std::function<void(DBClientCursorBatchIterator&)> f,
                             const NamespaceStringOrUUID& nsOrUuid,
                             Query query,
                             const BSONObj* fieldsToReturn = nullptr,
                             int queryOptions = QueryOption_Exhaust,
                             int batchSize = 0) override;


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

    virtual bool isReplicaSetMember() const = 0;

    virtual bool isMongos() const = 0;

    /**
     * Parses command replies and runs them through the metadata reader.
     * This is virtual and non-const to allow subclasses to act on failures.
     */
    virtual rpc::UniqueReply parseCommandReplyMessage(const std::string& host,
                                                      const Message& replyMsg);

    // This is only for DBClientCursor.
    static void (*withConnection_do_not_use)(std::string host, std::function<void(DBClientBase*)>);

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

    static AtomicWord<long long> ConnectionIdSequence;
    long long _connectionId;  // unique connection id for this connection

    std::vector<std::string> _saslMechsForAuth;

private:
    auth::RunCommandHook _makeAuthRunCommandHook();

    /**
     * The rpc protocols this client supports.
     *
     */
    rpc::ProtocolSet _clientRPCProtocols{rpc::supports::kAll};

    /**
     * The rpc protocol the remote server(s) support. We support 'opQueryOnly' by default unless
     * we detect support for OP_MSG at connection time.
     */
    rpc::ProtocolSet _serverRPCProtocols{rpc::supports::kOpQueryOnly};

    rpc::RequestMetadataWriter _metadataWriter;
    rpc::ReplyMetadataReader _metadataReader;

    enum QueryOptions _cachedAvailableOptions;
    bool _haveCachedAvailableOptions;
};  // DBClientBase

BSONElement getErrField(const BSONObj& result);
bool hasErrField(const BSONObj& result);

}  // namespace mongo
