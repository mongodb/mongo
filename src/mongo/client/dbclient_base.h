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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/client_api_version_parameters_gen.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/index_spec.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/read_preference.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/database_name.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/str.h"

namespace mongo {

namespace executor {
struct RemoteCommandResponse;
}

/**
 * Returns the database name portion of an ns std::string.
 */
std::string nsGetDB(const std::string& ns);

/**
 * Returns the collection name portion of an ns std::string.
 */
std::string nsGetCollection(const std::string& ns);

/**
 * Allows callers of the internal client 'find()' API below to request an exhaust cursor.
 *
 * Such cursors use a special OP_MSG facility under the hood. When exhaust is requested, the server
 * writes the full results of the query into the socket (split into getMore batches), without
 * waiting for explicit getMore requests from the client.
 */
enum class ExhaustMode { kOn, kOff };

/**
 * Abstract class that implements the core db operations.
 */
class DBClientBase {
    DBClientBase(const DBClientBase&) = delete;
    DBClientBase& operator=(const DBClientBase&) = delete;

public:
    static const uint64_t INVALID_SOCK_CREATION_TIME;

    DBClientBase(const ClientAPIVersionParameters* apiParameters = nullptr)
        : _logLevel(logv2::LogSeverity::Log()), _connectionId(ConnectionIdSequence.fetchAndAdd(1)) {
        if (apiParameters) {
            _apiParameters = *apiParameters;
        }
    }

    virtual ~DBClientBase() {}

    virtual std::string toString() const = 0;

    virtual std::string getServerAddress() const = 0;

    /**
     * Reconnect if needed and allowed.
     */
    virtual void ensureConnection() {}

    /**
     * If not checked recently, checks whether the underlying socket/sockets are still valid.
     */
    virtual bool isStillConnected() = 0;

    long long getConnectionId() const {
        return _connectionId;
    }

    /**
     * Returns true if this connection is currently in a failed state.
     */
    virtual bool isFailed() const = 0;

    virtual ConnectionString::ConnectionType type() const = 0;

    virtual double getSoTimeout() const = 0;

    virtual uint64_t getSockCreationMicroSec() const {
        return INVALID_SOCK_CREATION_TIME;
    }

    virtual void reset() {}

    /**
     * Returns true in isPrimary param if this db is the current primary of a replica pair.
     *
     * Pass in info for more details e.g.:
     *   { "isprimary" : 1.0 , "msg" : "not paired" , "ok" : 1.0  }
     *
     * Returns true if command invoked successfully.
     */
    virtual bool isPrimary(bool& isPrimary, BSONObj* info = nullptr);

    virtual bool isReplicaSetMember() const = 0;

    virtual bool isMongos() const = 0;

    virtual int getMinWireVersion() = 0;
    virtual int getMaxWireVersion() = 0;

    const std::vector<std::string>& getIsPrimarySaslMechanisms() const {
        return _saslMechsForAuth;
    }

    /**
     * Returns the latest operationTime tracked on this client.
     */
    Timestamp getOperationTime();

    void setOperationTime(Timestamp operationTime);

    /**
     * Sets a RequestMetadataWriter on this connection.
     *
     * TODO: support multiple metadata writers.
     */
    virtual void setRequestMetadataWriter(rpc::RequestMetadataWriter writer);

    /**
     * Gets the RequestMetadataWriter that is set on this connection. This may be an uninitialized
     * std::function, so it should be checked for validity with operator bool() first.
     */
    const rpc::RequestMetadataWriter& getRequestMetadataWriter();

    /**
     * Sets a ReplyMetadataReader on this connection.
     *
     * TODO: support multiple metadata readers.
     */
    virtual void setReplyMetadataReader(rpc::ReplyMetadataReader reader);

    /**
     * Gets the ReplyMetadataReader that is set on this connection. This may be an uninitialized
     * std::function, so it should be checked for validity with operator bool() first.
     */
    const rpc::ReplyMetadataReader& getReplyMetadataReader();

    /**
     * Parses command replies and runs them through the metadata reader.
     * This is virtual and non-const to allow subclasses to act on failures.
     */
    virtual rpc::UniqueReply parseCommandReplyMessage(const std::string& host,
                                                      const Message& replyMsg);

    /**
     * Runs the specified command request.
     */
    virtual std::pair<rpc::UniqueReply, DBClientBase*> runCommandWithTarget(OpMsgRequest request);

    /**
     * This shared_ptr overload is used to possibly return a shared_ptr to the replica set member
     * that the command was dispatched to. It's needed if the caller needs a lifetime for that
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

    /**
     * Runs a database command. Database commands are represented as BSON objects. Common database
     * commands have prebuilt helper functions -- see below. If a helper is not available you can
     * directly call runCommand.
     *
     *  'dbName': Database name. Use "admin" for global administrative commands.
     *  'cmd': The command object to execute. For example, { hello : 1 }.
     *  'info': The result object the database returns. Typically has { ok : ..., errmsg : ... }
     *          fields set.
     *  'options': See enum QueryOptions - normally not needed to run a command.
     *  'vts': optional validated tenancy scope used to include tenancy information on a command
     *
     *  Returns true if the command returned "ok".
     */
    bool runCommand(const DatabaseName& dbName, BSONObj cmd, BSONObj& info, int options = 0);

    /*
     * Wraps up the runCommand function avove, but returns the DBClient that actually ran the
     * command. When called against a replica set, this will return the specific replica set member
     * the command ran against.
     *
     * This is used in the shell so that cursors can send getMore through the correct connection.
     */
    std::tuple<bool, DBClientBase*> runCommandWithTarget(const DatabaseName& dbName,
                                                         BSONObj cmd,
                                                         BSONObj& info,
                                                         int options = 0);

    /**
     * Authenticates to another cluster member using appropriate authentication data.
     * Throws an exception if authentication fails.
     */
    virtual void authenticateInternalUser(
        auth::StepDownBehavior stepDownBehavior = auth::StepDownBehavior::kKillConnection);

    /**
     * Authenticates a user.
     *
     * The 'params' BSONObj should be initialized with some of the fields below. Which fields
     * are required depends on the mechanism, which is mandatory.
     *
     *  'mechanism': The std::string name of the sasl mechanism to use. Mandatory.
     *  'user': The std::string name of the user to authenticate. Mandatory.
     *  'db': The database target of the auth command, which identifies the location
     *        of the credential information for the user. May be "$external" if
     *        credential information is stored outside of the mongo cluster. Mandatory.
     *  'pwd': The password data.
     *  'serviceName': The GSSAPI service name to use. Defaults to "mongodb".
     *  'serviceHostname': The GSSAPI hostname to use. Defaults to the name of the remote host.
     *
     * Other fields in 'params' are silently ignored.
     * Returns normally on success, and throws on error. Throws a DBException with getCode() ==
     * ErrorCodes::AuthenticationFailed if authentication is rejected. All other exceptions are
     * tantamount to authentication failure, but may also indicate more serious problems.
     */
    void auth(const BSONObj& params);

    /**
     * Authorizes access to a particular database.
     *
     * Authentication is separate for each database on the server -- you may authenticate for any
     * number of databases on a single connection. The "admin" database is special and once
     * authenticated provides access to all databases on the server.
     */
    void auth(const DatabaseName& dbname, StringData username, StringData pwd);

    /**
     * Logs out the connection for the given database.
     *
     * 'dbname': The database to logout from.
     * 'info': The result object for the logout command (provided for backwards compatibility with
     *         mongo shell).
     */
    virtual void logout(const DatabaseName& dbname, BSONObj& info);

    virtual bool authenticatedDuringConnect() const {
        return false;
    }

    /**
     * Creates a new collection in the database. Normally, collection creation is automatic. You
     * would use this function if you wish to specify special options on creation.
     *
     *  If the collection already exists, no action occurs.
     *
     *  'nss': Fully qualified collection name.
     *  'size': Desired initial extent size for the collection.
     *          Must be <= 1000000000 for normal collections.
     *          For fixed size (capped) collections, this size is the total/max size of the
     *          collection.
     *  'capped': If true, this is a fixed size collection (where old data rolls out).
     *  'max': Maximum number of objects if capped (optional).
     *
     * Returns true if successful.
     */
    bool createCollection(const NamespaceString& nss,
                          long long size = 0,
                          bool capped = false,
                          int max = 0,
                          BSONObj* info = nullptr,
                          boost::optional<BSONObj> writeConcernObj = boost::none);

    /**
     * Deletes the specified collection.
     *
     *  'info': An optional output parameter that receives the result object the database returns
     *          from the drop command. May be null if the caller doesn't need that info.
     */
    virtual bool dropCollection(const NamespaceString& nss,
                                const WriteConcernOptions& writeConcern = WriteConcernOptions(),
                                BSONObj* info = nullptr) {
        auto coll = nss.coll();
        uassert(10011, "no collection name", coll.size());

        BSONObj temp;
        if (info == nullptr) {
            info = &temp;
        }

        bool res = runCommand(
            nss.dbName(), BSON("drop" << coll << "writeConcern" << writeConcern.toBSON()), *info);
        return res;
    }

    /**
     * { name : "<short collection name>",
     *   options : { }
     * }
     */
    std::list<BSONObj> getCollectionInfos(const DatabaseName& dbName,
                                          const BSONObj& filter = BSONObj());

    /**
     * Drops an entire database.
     */
    virtual bool dropDatabase(const DatabaseName& dbName,
                              const WriteConcernOptions& writeConcern = WriteConcernOptions(),
                              BSONObj* info = nullptr) {
        BSONObj o;
        if (info == nullptr)
            info = &o;
        return runCommand(
            dbName, BSON("dropDatabase" << 1 << "writeConcern" << writeConcern.toBSON()), *info);
    }

    /**
     * Lists databases available on the server.
     *
     * 'filter': A filter for the results
     * 'nameOnly': Only return the names of the databases
     * 'authorizedDatabases': Only return the databases the user is authorized on
     * 'useListDatabasesForAllTenants': Use command listDatabasesForAllTenants
     */
    std::vector<BSONObj> getDatabaseInfos(const BSONObj& filter = BSONObj(),
                                          bool nameOnly = false,
                                          bool authorizedDatabases = false,
                                          bool useListDatabasesForAllTenants = false);

    /**
     * Creates an index on the collection 'nss' as described by the given keys. If you wish to
     * specify options, see the more flexible overload of 'createIndex' which takes an IndexSpec
     * object. Failure to construct the index is reported by throwing a AssertionException.
     *
     *  'nss': NamespaceString on which to create the index
     *  'keys': Document describing keys and index types. You must provide at least one field and
     *          its direction.
     */
    void createIndex(const NamespaceString& nss,
                     const BSONObj& keys,
                     boost::optional<BSONObj> writeConcernObj = boost::none) {
        return createIndex(nss, IndexSpec().addKeys(keys), writeConcernObj);
    }

    /**
     * Creates an index on the collection 'nss' as described by the given descriptor. Failure to
     * construct the index is reported by throwing a AssertionException.
     *
     *  'nss': NamespaceString on which to create the index
     *  'descriptor': Configuration object describing the index to create. The descriptor must
     *                describe at least one key and index type.
     */
    virtual void createIndex(const NamespaceString& nss,
                             const IndexSpec& descriptor,
                             boost::optional<BSONObj> writeConcernObj = boost::none) {
        std::vector<const IndexSpec*> toBuild;
        toBuild.push_back(&descriptor);
        createIndexes(nss, toBuild, writeConcernObj);
    }

    virtual void createIndexes(const NamespaceString& nss,
                               const std::vector<const IndexSpec*>& descriptor,
                               boost::optional<BSONObj> writeConcernObj = boost::none);

    /**
     * Creates indexes on the collection 'nss' as described by 'specs'.
     *
     * Failure to construct the indexes is reported by throwing an AssertionException.
     */
    virtual void createIndexes(const NamespaceString& nss,
                               const std::vector<BSONObj>& specs,
                               boost::optional<BSONObj> writeConcernObj = boost::none);

    /**
     * Lists indexes on the collection 'nsOrUuid'.
     * Includes in-progress indexes.
     *
     * If 'includeBuildUUIDs' is true, in-progress index specs will have the following format:
     * {
     *     spec: <BSONObj>
     *     buildUUID: <UUID>
     * }
     * and ready index specs will only list the spec.
     *
     * If 'includeBuildUUIDs' is false, only the index spec will be returned without a way to
     * distinguish between ready and in-progress index specs.
     */
    virtual std::list<BSONObj> getIndexSpecs(const NamespaceStringOrUUID& nsOrUuid,
                                             bool includeBuildUUIDs,
                                             int options);

    virtual void dropIndex(const NamespaceString& nss,
                           BSONObj keys,
                           boost::optional<BSONObj> writeConcernObj = boost::none);
    virtual void dropIndex(const NamespaceString& nss,
                           const std::string& indexName,
                           boost::optional<BSONObj> writeConcernObj = boost::none);

    /**
     * Drops all indexes for the collection.
     */
    virtual void dropIndexes(const NamespaceString& nss,
                             boost::optional<BSONObj> writeConcernObj = boost::none);

    virtual void reIndex(const NamespaceString& nss);

    static std::string genIndexName(const BSONObj& keys);

    /**
     * Sends the provided message, returning the server's reponse.
     *
     * 'actualServer' is set to the actual server where they call went if there was a choice (for
     * example SecondaryOk).
     */
    Message call(Message& toSend, std::string* actualServer = nullptr) {
        return _call(toSend, actualServer);
    };

    virtual void say(Message& toSend,
                     bool isRetry = false,
                     std::string* actualServer = nullptr) = 0;

    /**
     * Used by QueryOption_Exhaust. To use that your subclass must implement this.
     */
    virtual Message recv(int lastRequestId) {
        MONGO_verify(false);
        uasserted(ErrorCodes::NotImplemented, "recv() not implemented");
    }

    /**
     * Issues a find command described by 'findRequest', and returns the resulting cursor.
     */
    virtual std::unique_ptr<DBClientCursor> find(FindCommandRequest findRequest,
                                                 const ReadPreferenceSetting& readPref,
                                                 ExhaustMode exhaustMode);

    /**
     * Convenience overloads. Identical to the 'find()' overload above, but default values of
     * "primary" read preference and 'ExhaustMode::kOff' are used when not supplied by the caller.
     */
    std::unique_ptr<DBClientCursor> find(FindCommandRequest findRequest) {
        ReadPreferenceSetting defaultReadPref{};
        return find(std::move(findRequest), defaultReadPref, ExhaustMode::kOff);
    }

    std::unique_ptr<DBClientCursor> find(FindCommandRequest findRequest,
                                         const ReadPreferenceSetting& readPref) {
        return find(std::move(findRequest), readPref, ExhaustMode::kOff);
    }

    /**
     * Issues a find command described by 'findRequest' and the given read preference. Rather than
     * returning a cursor to the caller, iterates the cursor under the hood and calls the provided
     * 'callback' function against each of the documents produced by the cursor.
     */
    void find(FindCommandRequest findRequest, std::function<void(const BSONObj&)> callback) {
        find(std::move(findRequest),
             ReadPreferenceSetting{},
             ExhaustMode::kOff,
             std::move(callback));
    }

    void find(FindCommandRequest findRequest,
              const ReadPreferenceSetting& readPref,
              ExhaustMode exhaustMode,
              std::function<void(const BSONObj&)> callback);

    /**
     * Issues a find command describe by 'findRequest', but augments the request to have a limit of
     * 1. It is illegal for the given 'findRequest' to have a limit already set.
     *
     * Returns the document resulting from the query, or an empty document if the query has no
     * results.
     */
    BSONObj findOne(FindCommandRequest findRequest, const ReadPreferenceSetting& readPref);

    /**
     * Identical to the 'findOne()' overload above, but uses a default value of "primary" for the
     * read preference.
     */
    BSONObj findOne(FindCommandRequest findRequest) {
        ReadPreferenceSetting defaultReadPref{};
        return findOne(std::move(findRequest), defaultReadPref);
    }

    /**
     * Issues a find command against the given collection (passed in either by namespace or by UUID)
     * with the given 'filter'. Also augments the find request to have a limit of 1.
     *
     * Returns the document resulting from the query, or an empty document if the query has no
     * results.
     */
    BSONObj findOne(const NamespaceStringOrUUID& nssOrUuid, BSONObj filter);

    /**
     * Don't use this - called automatically by DBClientCursor for you.
     *   'cursorId': Id of cursor to retrieve.
     *   Returns an handle to a previously allocated cursor.
     *   Throws AssertionException.
     */
    virtual std::unique_ptr<DBClientCursor> getMore(const NamespaceString& nss, long long cursorId);

    /**
     * Counts number of objects in collection ns that match the query criteria specified.
     * Throws UserAssertion if database returns an error.
     */
    virtual long long count(NamespaceStringOrUUID nsOrUuid,
                            const BSONObj& query = BSONObj(),
                            int options = 0,
                            int limit = 0,
                            int skip = 0,
                            boost::optional<BSONObj> readConcernObj = boost::none);

    /**
     * Executes an acknowledged command to insert a vector of documents.
     */
    virtual BSONObj insertAcknowledged(const NamespaceString& nss,
                                       const std::vector<BSONObj>& v,
                                       bool ordered = true,
                                       boost::optional<BSONObj> writeConcernObj = boost::none);

    /**
     * Executes a fire-and-forget command to insert a single document.
     */
    virtual void insert(const NamespaceString& nss,
                        BSONObj obj,
                        bool ordered = true,
                        boost::optional<BSONObj> writeConcernObj = boost::none);

    /**
     * Executes a fire-and-forget command to insert a vector of documents.
     */
    virtual void insert(const NamespaceString& nss,
                        const std::vector<BSONObj>& v,
                        bool ordered = true,
                        boost::optional<BSONObj> writeConcernObj = boost::none);

    /**
     * Executes an acknowledged command to update the objects that match the query.
     */
    virtual BSONObj updateAcknowledged(const NamespaceString& nss,
                                       const BSONObj& filter,
                                       BSONObj updateSpec,
                                       bool upsert = false,
                                       bool multi = false,
                                       boost::optional<BSONObj> writeConcernObj = boost::none);

    /**
     * Executes a fire-and-forget command to update the objects that match the query.
     */
    virtual void update(const NamespaceString& nss,
                        const BSONObj& filter,
                        BSONObj updateSpec,
                        bool upsert = false,
                        bool multi = false,
                        boost::optional<BSONObj> writeConcernObj = boost::none);

    /**
     * Executes an acknowledged command to remove the objects that match the query.
     */
    virtual BSONObj removeAcknowledged(const NamespaceString& nss,
                                       const BSONObj& filter,
                                       bool removeMany = true,
                                       boost::optional<BSONObj> writeConcernObj = boost::none);

    /**
     * Executes a fire-and-forget command to remove the objects that match the query.
     */
    virtual void remove(const NamespaceString& nss,
                        const BSONObj& filter,
                        bool removeMany = true,
                        boost::optional<BSONObj> writeConcernObj = boost::none);

    virtual void killCursor(const NamespaceString& nss, long long cursorID);

    // This is only for DBClientCursor.
    static void (*withConnection_do_not_use)(std::string host, std::function<void(DBClientBase*)>);

#ifdef MONGO_CONFIG_SSL
    /**
     * Gets the SSL configuration of this client.
     */
    virtual const SSLConfiguration* getSSLConfiguration() = 0;

    /**
     * Returns true if this client was connected using transient SSL parameters. May return
     * false if this client was never connected.
     */
    virtual bool isUsingTransientSSLParams() const {
        return false;
    }

    virtual bool isTLS() = 0;
#else
    virtual bool isTLS() {
        return false;
    }
#endif

    virtual bool isGRPC() {
        return false;
    }

    const ClientAPIVersionParameters& getApiParameters() const {
        return _apiParameters;
    }

    void setShouldThrowOnStaleConfigError(bool value) {
        _shouldThrowOnStaleConfigError = value;
    }

protected:
    /**
     * Generates the validated tenancy scope for internal requests utilized within the server or
     * across servers.
     */
    virtual auth::ValidatedTenancyScope _createInnerRequestVTS(
        const boost::optional<TenantId>& tenantId) const;

protected:
    /**
     * Returns true if the result of a command is ok.
     */
    bool isOk(const BSONObj&);

    /**
     * Returns true if the element contains a not primary error.
     */
    bool isNotPrimaryErrorString(const BSONElement& e);

    BSONObj _countCmd(NamespaceStringOrUUID nsOrUuid,
                      const BSONObj& query,
                      int options,
                      int limit,
                      int skip,
                      boost::optional<BSONObj> readConcernObj);

    virtual void _auth(const BSONObj& params);

    /**
     * Controls how chatty the client is about network errors & such. See log.h.
     */
    const logv2::LogSeverity _logLevel;

    static AtomicWord<long long> ConnectionIdSequence;
    long long _connectionId;  // unique connection id for this connection

    std::vector<std::string> _saslMechsForAuth;

private:
    virtual Message _call(Message& toSend, std::string* actualServer) = 0;

    /**
     * Implementation for getIndexes() and getReadyIndexes().
     */
    std::list<BSONObj> _getIndexSpecs(const NamespaceStringOrUUID& nsOrUuid,
                                      const BSONObj& cmd,
                                      int options);

    auth::RunCommandHook _makeAuthRunCommandHook();

    // Unless explicitly opted out, a DBClientBase should throw on a StaleConfig error so that the
    // error can be handled internally if applicable rather than propagated to the external client
    // right away.
    bool _shouldThrowOnStaleConfigError = true;

    rpc::RequestMetadataWriter _metadataWriter;
    rpc::ReplyMetadataReader _metadataReader;

    // The operationTime associated with the last command handled by the client.
    Timestamp _lastOperationTime;

    ClientAPIVersionParameters _apiParameters;
};  // DBClientBase

BSONElement getErrField(const BSONObj& result);
bool hasErrField(const BSONObj& result);

/*
 * RAII-style class to set new RequestMetadataWriter and ReplyMetadataReader on DBClientConnection
 * "_conn". On object destruction, '_conn' is set back to it's old RequestsMetadataWriter and
 * ReplyMetadataReader.
 */
class ScopedMetadataWriterAndReader {
    ScopedMetadataWriterAndReader(const ScopedMetadataWriterAndReader&) = delete;
    ScopedMetadataWriterAndReader& operator=(const ScopedMetadataWriterAndReader&) = delete;

public:
    ScopedMetadataWriterAndReader(DBClientBase* conn,
                                  rpc::RequestMetadataWriter writer,
                                  rpc::ReplyMetadataReader reader)
        : _conn(conn),
          _oldWriter(conn->getRequestMetadataWriter()),
          _oldReader(conn->getReplyMetadataReader()) {
        _conn->setRequestMetadataWriter(std::move(writer));
        _conn->setReplyMetadataReader(std::move(reader));
    }
    ~ScopedMetadataWriterAndReader() {
        _conn->setRequestMetadataWriter(std::move(_oldWriter));
        _conn->setReplyMetadataReader(std::move(_oldReader));
    }

private:
    DBClientBase* const _conn;  // not owned.
    rpc::RequestMetadataWriter _oldWriter;
    rpc::ReplyMetadataReader _oldReader;
};

}  // namespace mongo
