/**
 *    Copyright (C) 2009-2016 MongoDB Inc.
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

#include <string>
#include <vector>

#include "mongo/base/counter.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/write_concern.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/rpc/request_interface.h"
#include "mongo/util/string_map.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;
class Client;
class OperationContext;
class Timer;

namespace mutablebson {
class Document;
}  // namespace mutablebson

namespace rpc {
class ServerSelectionMetadata;
}  // namespace rpc

/**
 * Serves as a base for server commands. See the constructor for more details.
 */
class Command {
protected:
    // The type of the first field in 'cmdObj' must be mongo::String. The first field is
    // interpreted as a collection name.
    static std::string parseNsFullyQualified(const std::string& dbname, const BSONObj& cmdObj);

    // The type of the first field in 'cmdObj' must be mongo::String or Symbol.
    // The first field is interpreted as a collection name.
    static NamespaceString parseNsCollectionRequired(const std::string& dbname,
                                                     const BSONObj& cmdObj);

public:
    typedef StringMap<Command*> CommandMap;

    enum class ReadWriteType { kCommand, kRead, kWrite };

    /**
     * Constructs a new command and causes it to be registered with the global commands list. It is
     * not safe to construct commands other than when the server is starting up.
     *
     * @param webUI expose the command in the web ui as localhost:28017/<name>
     * @param oldName an optional old, deprecated name for the command
     */
    Command(StringData name, bool webUI = false, StringData oldName = StringData());

    // NOTE: Do not remove this declaration, or relocate it in this class. We
    // are using this method to control where the vtable is emitted.
    virtual ~Command();

    /**
     * Returns the command's name. This value never changes for the lifetime of the command.
     */
    const std::string& getName() const {
        return _name;
    }

    /**
     * Returns whether this command is visible in the Web UI.
     */
    bool isWebUI() const {
        return _webUI;
    }

    // Return the namespace for the command. If the first field in 'cmdObj' is of type
    // mongo::String, then that field is interpreted as the collection name, and is
    // appended to 'dbname' after a '.' character. If the first field is not of type
    // mongo::String, then 'dbname' is returned unmodified.
    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const;

    // Utility that returns a ResourcePattern for the namespace returned from
    // parseNs(dbname, cmdObj).  This will be either an exact namespace resource pattern
    // or a database resource pattern, depending on whether parseNs returns a fully qualifed
    // collection name or just a database name.
    ResourcePattern parseResourcePattern(const std::string& dbname, const BSONObj& cmdObj) const;

    virtual std::size_t reserveBytesForReply() const {
        return 0u;
    }

    /* run the given command
       implement this...

       return value is true if succeeded.  if false, set errmsg text.
    */
    virtual bool run(OperationContext* txn,
                     const std::string& db,
                     BSONObj& cmdObj,
                     int options,
                     std::string& errmsg,
                     BSONObjBuilder& result) = 0;

    /**
     * Translation point between the new request/response types and the legacy types.
     *
     * Then we won't need to mutate the command object. At that point we can also make
     * this method virtual so commands can override it directly.
     */
    bool run(OperationContext* txn,
             const rpc::RequestInterface& request,
             rpc::ReplyBuilderInterface* replyBuilder);

    /**
     * supportsWriteConcern returns true if this command should be parsed for a writeConcern
     * field and wait for that write concern to be satisfied after the command runs.
     *
     * @param cmd is a BSONObj representation of the command that is used to determine if the
     *            the command supports a write concern. Ex. aggregate only supports write concern
     *            when $out is provided.
     */
    virtual bool supportsWriteConcern(const BSONObj& cmd) const = 0;

    /* Return true if only the admin ns has privileges to run this command. */
    virtual bool adminOnly() const {
        return false;
    }

    /* Like adminOnly, but even stricter: we must either be authenticated for admin db,
       or, if running without auth, on the local interface.  Used for things which
       are so major that remote invocation may not make sense (e.g., shutdownServer).

       When localHostOnlyIfNoAuth() is true, adminOnly() must also be true.
    */
    virtual bool localHostOnlyIfNoAuth(const BSONObj& cmdObj) {
        return false;
    }

    /* Return true if slaves are allowed to execute the command
    */
    virtual bool slaveOk() const = 0;

    /* Return true if the client force a command to be run on a slave by
       turning on the 'slaveOk' option in the command query.
    */
    virtual bool slaveOverrideOk() const {
        return false;
    }

    /**
     * Override and return fales if the command opcounters should not be incremented on
     * behalf of this command.
     */
    virtual bool shouldAffectCommandCounter() const {
        return true;
    }

    virtual void help(std::stringstream& help) const;

    /**
     * Commands which can be explained override this method. Any operation which has a query
     * part and executes as a tree of execution stages can be explained. A command should
     * implement explain by:
     *
     *   1) Calling its custom parse function in order to parse the command. The output of
     *   this function should be a CanonicalQuery (representing the query part of the
     *   operation), and a PlanExecutor which wraps the tree of execution stages.
     *
     *   2) Calling Explain::explainStages(...) on the PlanExecutor. This is the function
     *   which knows how to convert an execution stage tree into explain output.
     *
     * TODO: Remove the 'serverSelectionMetadata' parameter in favor of reading the
     * ServerSelectionMetadata off 'txn'. Once OP_COMMAND is implemented in mongos, this metadata
     * will be parsed and attached as a decoration on the OperationContext, as is already done on
     * the mongod side.
     */
    virtual Status explain(OperationContext* txn,
                           const std::string& dbname,
                           const BSONObj& cmdObj,
                           ExplainCommon::Verbosity verbosity,
                           const rpc::ServerSelectionMetadata& serverSelectionMetadata,
                           BSONObjBuilder* out) const;

    /**
     * Checks if the given client is authorized to run this command on database "dbname"
     * with the invocation described by "cmdObj".
     */
    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj);

    /**
     * Redacts "cmdObj" in-place to a form suitable for writing to logs.
     *
     * The default implementation does nothing.
     */
    virtual void redactForLogging(mutablebson::Document* cmdObj);

    /**
     * Returns a copy of "cmdObj" in a form suitable for writing to logs.
     * Uses redactForLogging() to transform "cmdObj".
     */
    BSONObj getRedactedCopyForLogging(const BSONObj& cmdObj);

    /* Return true if a replica set secondary should go into "recovering"
       (unreadable) state while running this command.
     */
    virtual bool maintenanceMode() const {
        return false;
    }

    /* Return true if command should be permitted when a replica set secondary is in "recovering"
       (unreadable) state.
     */
    virtual bool maintenanceOk() const {
        return true; /* assumed true prior to commit */
    }

    /**
     * Returns true if this Command supports the readConcern argument.
     *
     * If the readConcern argument is sent to a command that returns false the command processor
     * will reject the command, returning an appropriate error message. For commands that support
     * the argument, the command processor will instruct the RecoveryUnit to only return
     * "committed" data, failing if this isn't supported by the storage engine.
     *
     * Note that this is never called on mongos. Sharded commands are responsible for forwarding
     * the option to the shards as needed. We rely on the shards to fail the commands in the
     * cases where it isn't supported.
     */
    virtual bool supportsReadConcern() const {
        return false;
    }

    virtual LogicalOp getLogicalOp() const {
        return LogicalOp::opCommand;
    }

    /**
     * Returns whether this operation is a read, write, or command.
     *
     * Commands which implement database read or write logic should override this to return kRead
     * or kWrite as appropriate.
     */
    virtual ReadWriteType getReadWriteType() const {
        return ReadWriteType::kCommand;
    }

protected:
    /**
     * Appends to "*out" the privileges required to run this command on database "dbname" with
     * the invocation described by "cmdObj".  New commands shouldn't implement this, they should
     * implement checkAuthForCommand instead.
     */
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        // The default implementation of addRequiredPrivileges should never be hit.
        fassertFailed(16940);
    }

    static CommandMap* _commands;
    static CommandMap* _commandsByBestName;

    // Counters for how many times this command has been executed and failed
    Counter64 _commandsExecuted;
    Counter64 _commandsFailed;

public:
    static const CommandMap* commandsByBestName() {
        return _commandsByBestName;
    }

    // Counter for unknown commands
    static Counter64 unknownCommands;

    static Command* findCommand(StringData name);

    /**
     * Executes a command after stripping metadata, performing authorization checks,
     * handling audit impersonation, and (potentially) setting maintenance mode. This method
     * also checks that the command is permissible to run on the node given its current
     * replication state. All the logic here is independent of any particular command; any
     * functionality relevant to a specific command should be confined to its run() method.
     *
     * This is currently used by mongod and dbwebserver.
     */
    static void execCommand(OperationContext* txn,
                            Command* command,
                            const rpc::RequestInterface& request,
                            rpc::ReplyBuilderInterface* replyBuilder);

    // For mongos
    // TODO: remove this entirely now that all instances of ClientBasic are instances
    // of Client. This will happen as part of SERVER-18292
    static void execCommandClientBasic(OperationContext* txn,
                                       Command* c,
                                       Client& client,
                                       int queryOptions,
                                       const char* ns,
                                       BSONObj& cmdObj,
                                       BSONObjBuilder& result);

    // Helper for setting errmsg and ok field in command result object.
    static void appendCommandStatus(BSONObjBuilder& result, bool ok, const std::string& errmsg);

    // @return s.isOK()
    static bool appendCommandStatus(BSONObjBuilder& result, const Status& status);

    /**
     * Helper for setting a writeConcernError field in the command result object if
     * a writeConcern error occurs.
     *
     * @param result is the BSONObjBuilder for the command response. This function creates the
     *               writeConcernError field for the response.
     * @param awaitReplicationStatus is the status received from awaitReplication.
     * @param wcResult is the writeConcernResult object that holds other write concern information.
     *      This is primarily used for populating errInfo when a timeout occurs, and is populated
     *      by waitForWriteConcern.
     */
    static void appendCommandWCStatus(BSONObjBuilder& result,
                                      const Status& awaitReplicationStatus,
                                      const WriteConcernResult& wcResult = WriteConcernResult());

    /**
     * If true, then testing commands are available. Defaults to false.
     *
     * Testing commands should conditionally register themselves by consulting this flag:
     *
     *     MONGO_INITIALIZER(RegisterMyTestCommand)(InitializerContext* context) {
     *         if (Command::testCommandsEnabled) {
     *             // Leaked intentionally: a Command registers itself when constructed.
     *             new MyTestCommand();
     *         }
     *         return Status::OK();
     *     }
     *
     * To make testing commands available by default, change the value to true before running any
     * mongo initializers:
     *
     *     int myMain(int argc, char** argv, char** envp) {
     *         static StaticObserver StaticObserver;
     *         Command::testCommandsEnabled = true;
     *         ...
     *         runGlobalInitializersOrDie(argc, argv, envp);
     *         ...
     *     }
     */
    static bool testCommandsEnabled;

    /**
     * Returns true if this a request for the 'help' information associated with the command.
     */
    static bool isHelpRequest(const BSONElement& helpElem);

    static const char kHelpFieldName[];

    /**
     * Generates a reply from the 'help' information associated with a command. The state of
     * the passed ReplyBuilder will be in kOutputDocs after calling this method.
     */
    static void generateHelpResponse(OperationContext* txn,
                                     const rpc::RequestInterface& request,
                                     rpc::ReplyBuilderInterface* replyBuilder,
                                     const Command& command);

    /**
     * When an assertion is hit during command execution, this method is used to fill the fields
     * of the command reply with the information from the error. In addition, information about
     * the command is logged. This function does not return anything, because there is typically
     * already an active exception when this function is called, so there
     * is little that can be done if it fails.
     */
    static void generateErrorResponse(OperationContext* txn,
                                      rpc::ReplyBuilderInterface* replyBuilder,
                                      const DBException& exception,
                                      const rpc::RequestInterface& request,
                                      Command* command,
                                      const BSONObj& metadata);

    /**
     * Generates a command error response. This overload of generateErrorResponse is intended
     * to be called if the command is successfully parsed, but there is an error before we have
     * a handle to the actual Command object. This can happen, for example, when the command
     * is not found.
     */
    static void generateErrorResponse(OperationContext* txn,
                                      rpc::ReplyBuilderInterface* replyBuilder,
                                      const DBException& exception,
                                      const rpc::RequestInterface& request);

    /**
     * Generates a command error response. Similar to other overloads of generateErrorResponse,
     * but doesn't print any information about the specific command being executed. This is
     * neccessary, for example, if there is
     * an assertion hit while parsing the command.
     */
    static void generateErrorResponse(OperationContext* txn,
                                      rpc::ReplyBuilderInterface* replyBuilder,
                                      const DBException& exception);

    /**
     * Records the error on to the OperationContext. This hook is needed because mongos
     * does not have CurOp linked in to it.
     */
    static void registerError(OperationContext* txn, const DBException& exception);

    /**
     * This function checks if a command is a user management command by name.
     */
    static bool isUserManagementCommand(const std::string& name);

private:
    /**
     * Checks to see if the client is authorized to run the given command with the given
     * parameters on the given named database.
     *
     * Returns Status::OK() if the command is authorized.  Most likely returns
     * ErrorCodes::Unauthorized otherwise, but any return other than Status::OK implies not
     * authorized.
     */
    static Status _checkAuthorization(Command* c,
                                      Client* client,
                                      const std::string& dbname,
                                      const BSONObj& cmdObj);

    // The full name of the command
    const std::string _name;

    // Whether the command is available in the web UI
    const bool _webUI;

    // Pointers to hold the metrics tree references
    ServerStatusMetricField<Counter64> _commandsExecutedMetric;
    ServerStatusMetricField<Counter64> _commandsFailedMetric;
};

}  // namespace mongo
