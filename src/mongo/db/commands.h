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

#include <boost/optional.hpp>
#include <fmt/format.h>
#include <functional>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/request_execution_context.h"
#include "mongo/db/write_concern.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/transport/service_executor.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/string_map.h"

namespace mongo {

extern FailPoint failCommand;
extern FailPoint waitInCommandMarkKillOnClientDisconnect;
extern const OperationContext::Decoration<boost::optional<BSONArray>> errorLabelsOverride;
extern const std::set<std::string> kNoApiVersions;
extern const std::set<std::string> kApiVersions1;

class AuthorizationContract;
class Command;
class CommandInvocation;
class OperationContext;

namespace mutablebson {
class Document;
}  // namespace mutablebson

/**
 * A simple set of type-erased hooks for pre and post command actions.
 *
 * These hooks will only run on external requests that form CommandInvocations (a.k.a. OP_MSG
 * requests). They are not applied for runCommandDirectly() or raw CommandInvocation::run() calls.
 */
class CommandInvocationHooks {
public:
    /**
     * Set `hooks` as the `CommandInvocationHooks` decoration of `serviceContext`
     */
    static void set(ServiceContext* serviceContext, std::unique_ptr<CommandInvocationHooks> hooks);

    virtual ~CommandInvocationHooks() = default;

    /**
     * A behavior to perform before CommandInvocation::run()
     */
    virtual void onBeforeRun(OperationContext* opCtx,
                             const OpMsgRequest& request,
                             CommandInvocation* invocation) = 0;

    /**
     * A behavior to perform before CommandInvocation::asyncRun(). Defaults to `onBeforeRun(...)`.
     */
    virtual void onBeforeAsyncRun(std::shared_ptr<RequestExecutionContext> rec,
                                  CommandInvocation* invocation) {
        onBeforeRun(rec->getOpCtx(), rec->getRequest(), invocation);
    }

    /**
     * A behavior to perform after CommandInvocation::run(). Note that the response argument is not
     * const, because the ReplyBuilderInterface does not expose any const methods to inspect the
     * response body. However, onAfterRun must not mutate the response body.
     */
    virtual void onAfterRun(OperationContext* opCtx,
                            const OpMsgRequest& request,
                            CommandInvocation* invocation,
                            rpc::ReplyBuilderInterface* response) = 0;

    /**
     * A behavior to perform after CommandInvocation::asyncRun(). Defaults to `onAfterRun(...)`.
     */
    virtual void onAfterAsyncRun(std::shared_ptr<RequestExecutionContext> rec,
                                 CommandInvocation* invocation) {
        onAfterRun(rec->getOpCtx(), rec->getRequest(), invocation, rec->getReplyBuilder());
    }
};

// Various helpers unrelated to any single command or to the command registry.
// Would be a namespace, but want to keep it closed rather than open.
// Some of these may move to the BasicCommand shim if they are only for legacy implementations.
struct CommandHelpers {
    static const WriteConcernOptions kMajorityWriteConcern;

    // The type of the first field in 'cmdObj' must be mongo::String. The first field is
    // interpreted as a collection name.
    static std::string parseNsFullyQualified(const BSONObj& cmdObj);

    // The type of the first field in 'cmdObj' must be mongo::String or Symbol.
    // The first field is interpreted as a collection name.
    static NamespaceString parseNsCollectionRequired(const DatabaseName& dbName,
                                                     const BSONObj& cmdObj);

    static NamespaceStringOrUUID parseNsOrUUID(const DatabaseName& dbName, const BSONObj& cmdObj);

    /**
     * Return the namespace for the command. If the first field in 'cmdObj' is of type
     * mongo::String, then that field is interpreted as the collection name, and is
     * appended to 'dbname' after a '.' character. If the first field is not of type
     * mongo::String, then 'dbname' is returned unmodified.
     */
    // TODO SERVER-68423: Remove this method once all call sites have been updated to pass
    // DatabaseName.
    static std::string parseNsFromCommand(StringData dbname, const BSONObj& cmdObj);

    /**
     * Return the namespace for the command. If the first field in 'cmdObj' is of type
     * mongo::String, then that field is interpreted as the collection name.
     * If the first field is not of type mongo::String, then the namespace only has database name.
     */
    static NamespaceString parseNsFromCommand(const DatabaseName& dbName, const BSONObj& cmdObj);

    /**
     * Utility that returns a ResourcePattern for the namespace returned from
     * BasicCommand::parseNs(dbname, cmdObj). This will be either an exact namespace resource
     * pattern or a database resource pattern, depending on whether parseNs returns a fully qualifed
     * collection name or just a database name.
     */
    static ResourcePattern resourcePatternForNamespace(const std::string& ns);

    static Command* findCommand(StringData name);

    /**
     * Helper for setting errmsg and ok field in command result object.
     *
     * This should generally only be called from the command dispatch code or to finish off the
     * result of serializing a reply BSONObj in the case when it isn't going directly into a real
     * command reply to be returned to the user.
     */
    static void appendSimpleCommandStatus(BSONObjBuilder& result,
                                          bool ok,
                                          const std::string& errmsg = {});

    /**
     * Adds the status fields to command replies.
     *
     * Calling this inside of commands to produce their reply is now deprecated. Just throw instead.
     */
    static bool appendCommandStatusNoThrow(BSONObjBuilder& result, const Status& status);

    /**
     * If "ok" field is present in `reply`, uses its truthiness.
     * Otherwise, the absence of failure is considered success, `reply` is patched to indicate it.
     * Returns true if reply indicates a success.
     */
    static bool extractOrAppendOk(BSONObjBuilder& reply);

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
     * Forward generic arguments from a client request to shards.
     */
    static BSONObj appendGenericCommandArgs(const BSONObj& cmdObjWithGenericArgs,
                                            const BSONObj& request);

    /**
     * Forward generic reply fields from a shard's reply to the client.
     */
    static void appendGenericReplyFields(const BSONObj& replyObjWithGenericReplyFields,
                                         const BSONObj& reply,
                                         BSONObjBuilder* replyBuilder);
    static BSONObj appendGenericReplyFields(const BSONObj& replyObjWithGenericReplyFields,
                                            const BSONObj& reply);

    /**
     * Returns a copy of 'cmdObj' with a majority writeConcern appended.  If the command object does
     * not contain a writeConcern, 'defaultWC' will be used instead, if supplied.
     */
    static BSONObj appendMajorityWriteConcern(
        const BSONObj& cmdObj, WriteConcernOptions defaultWC = WriteConcernOptions());

    /**
     * Rewrites cmdObj into a format safe to blindly forward to shards.
     *
     * This performs 2 transformations:
     * 1) $readPreference fields are moved into a subobject called $queryOptions. This matches the
     *    "wrapped" format historically used internally by mongos. Moving off of that format will be
     *    done as SERVER-29091.
     *
     * 2) Filter out generic arguments that shouldn't be blindly passed to the shards.  This is
     *    necessary because many mongos implementations of Command::run() just pass cmdObj through
     *    directly to the shards. However, some of the generic arguments fields are automatically
     *    appended in the egress layer. Removing them here ensures that they don't get duplicated.
     *
     * Ideally this function can be deleted once mongos run() implementations are more careful about
     * what they send to the shards.
     */
    static BSONObj filterCommandRequestForPassthrough(const BSONObj& cmdObj);
    static void filterCommandRequestForPassthrough(BSONObjIterator* cmdIter,
                                                   BSONObjBuilder* requestBuilder);

    /**
     * Rewrites reply into a format safe to blindly forward from shards to clients.
     *
     * Ideally this function can be deleted once mongos run() implementations are more careful about
     * what they return from the shards.
     */
    static BSONObj filterCommandReplyForPassthrough(const BSONObj& reply);
    static void filterCommandReplyForPassthrough(const BSONObj& reply, BSONObjBuilder* output);

    /**
     * Returns true if this a request for the 'help' information associated with the command.
     */
    static bool isHelpRequest(const BSONElement& helpElem);

    /**
     * Runs a command directly and returns the result. Does not do any other work normally handled
     * by command dispatch, such as checking auth, dealing with CurOp or waiting for write concern.
     * It is illegal to call this if the command does not exist.
     */
    static BSONObj runCommandDirectly(OperationContext* opCtx, const OpMsgRequest& request);

    /**
     * Runs the command synchronously in presence of a dedicated thread.
     * Otherwise, runs the command asynchronously.
     */
    static Future<void> runCommandInvocation(std::shared_ptr<RequestExecutionContext> rec,
                                             std::shared_ptr<CommandInvocation> invocation,
                                             bool useDedicatedThread);

    /**
     * Runs a previously parsed CommandInvocation and propagates the result to the
     * ReplyBuilderInterface. This function is agnostic to the derived type of the CommandInvocation
     * but may mirror, forward, or do other supplementary actions with the request.
     */
    static void runCommandInvocation(OperationContext* opCtx,
                                     const OpMsgRequest& request,
                                     CommandInvocation* invocation,
                                     rpc::ReplyBuilderInterface* response);

    /**
     * Runs a previously parsed command and propagates the result to the ReplyBuilderInterface. For
     * commands that do not offer an implementation tailored for asynchronous execution, the future
     * schedules the execution of the default implementation, historically designed for synchronous
     * execution.
     */
    static Future<void> runCommandInvocationAsync(std::shared_ptr<RequestExecutionContext> rec,
                                                  std::shared_ptr<CommandInvocation> invocation);

    /**
     * If '!invocation', we're logging about a Command pre-parse. It has to punt on the logged
     * namespace, giving only the request's $db. Since the Command hasn't parsed the request body,
     * we can't know the collection part of that namespace, so we leave it blank in the audit log.
     */
    static void auditLogAuthEvent(OperationContext* opCtx,
                                  const CommandInvocation* invocation,
                                  const OpMsgRequest& request,
                                  ErrorCodes::Error err);

    static void uassertNoDocumentSequences(StringData commandName, const OpMsgRequest& request);

    /**
     * Should be called before trying to Command::parse a request. Throws 'Unauthorized',
     * and emits an audit log entry, as an early failure if the calling client can't invoke that
     * Command. Returns true if no more auth checks should be performed.
     */
    static bool uassertShouldAttemptParse(OperationContext* opCtx,
                                          const Command* command,
                                          const OpMsgRequest& request);

    /**
     * Asserts that a majority write concern was used for a command.
     */
    static void uassertCommandRunWithMajority(StringData commandName,
                                              const WriteConcernOptions& wc);

    /**
     * Verifies that command is allowed to run under a transaction in the given database or
     * namespace, and throws if that verification doesn't pass.
     */
    static void canUseTransactions(const NamespaceString& nss,
                                   StringData cmdName,
                                   bool allowTransactionsOnConfigDatabase);

    static constexpr StringData kHelpFieldName = "help"_sd;

    /**
     * Checks if the command passed in is in the list of failCommands defined in the fail point.
     */
    static bool shouldActivateFailCommandFailPoint(const BSONObj& data,
                                                   const CommandInvocation* invocation,
                                                   Client* client);

    /**
     * Checks if the command passed in is in the list of failCommands defined in the fail point.
     */
    static bool shouldActivateFailCommandFailPoint(const BSONObj& data,
                                                   const NamespaceString& nss,
                                                   const Command* cmd,
                                                   Client* client);

    /**
     * Possibly uasserts according to the "failCommand" fail point.
     */
    static void evaluateFailCommandFailPoint(OperationContext* opCtx,
                                             const CommandInvocation* invocation);

    /**
     * Handles marking kill on client disconnect.
     */
    static void handleMarkKillOnClientDisconnect(OperationContext* opCtx,
                                                 bool shouldMarkKill = true);

    /**
     * Provides diagnostics if the reply builder contains an internal-only error, and will cause
     * deferred-fatality when testing diagnostics is enabled.
     */
    static void checkForInternalError(rpc::ReplyBuilderInterface* replyBuilder,
                                      bool isInternalClient);
};

/**
 * Serves as a base for server commands. See the constructor for more details.
 */
class Command {
public:
    using CommandMap = StringMap<Command*>;
    enum class AllowedOnSecondary { kAlways, kNever, kOptIn };
    enum class HandshakeRole { kNone, kHello, kAuth };

    /**
     * Constructs a new command and causes it to be registered with the global commands list. It is
     * not safe to construct commands other than when the server is starting up.
     *
     * @param oldName an old, deprecated name for the command
     */
    Command(StringData name, StringData oldName)
        : Command(name, std::vector<StringData>({oldName})) {}

    /**
     * @param aliases the optional list of aliases (e.g., old names) for the command
     */
    Command(StringData name, std::vector<StringData> aliases = {});

    Command(const Command&) = delete;
    Command& operator=(const Command&) = delete;

    // Do not remove or relocate the definition of this "key function".
    // See https://gcc.gnu.org/wiki/VerboseDiagnostics#missing_vtable
    virtual ~Command();

    virtual std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                                     const OpMsgRequest& request) = 0;

    virtual std::unique_ptr<CommandInvocation> parseForExplain(
        OperationContext* opCtx,
        const OpMsgRequest& request,
        boost::optional<ExplainOptions::Verbosity> explainVerbosity) {
        return parse(opCtx, request);
    }

    /**
     * Returns the command's name. This value never changes for the lifetime of the command.
     */
    const std::string& getName() const {
        return _name;
    }

    /**
     * Used by command implementations to hint to the rpc system how much space they will need in
     * their replies.
     */
    virtual std::size_t reserveBytesForReply() const {
        return 0u;
    }

    /**
     * Return true if only the admin ns has privileges to run this command.
     */
    virtual bool adminOnly() const {
        return false;
    }

    /**
     * Returns the role this command has in the connection handshake.
     */
    virtual HandshakeRole handshakeRole() const {
        return HandshakeRole::kNone;
    }

    /*
     * Returns the list of API versions that include this command.
     */
    virtual const std::set<std::string>& apiVersions() const;

    /*
     * Returns the list of API versions in which this command is deprecated.
     */
    virtual const std::set<std::string>& deprecatedApiVersions() const;

    /*
     * Some commands permit any values for apiVersion, apiStrict, and apiDeprecationErrors.
     * For internal (server to server) commands we should skip checking api version.
     */
    virtual bool skipApiVersionCheck() const {
        return false;
    }

    /**
     * Like adminOnly, but even stricter: we must either be authenticated for admin db,
     * or, if running without auth, on the local interface.  Used for things which
     * are so major that remote invocation may not make sense (e.g., shutdownServer).
     *
     * When localHostOnlyIfNoAuth() is true, adminOnly() must also be true.
     */
    virtual bool localHostOnlyIfNoAuth() const {
        return false;
    }

    /**
     * Note that secondaryAllowed should move to CommandInvocation but cannot because there is
     * one place (i.e. 'listCommands') that inappropriately produces the "secondaryOk" and
     * "secondaryOverrideOk" fields for each Command without regard to payload. This is
     * inappropriate because for some Commands (e.g. 'aggregate'), these properties depend
     * on request payload. See SERVER-34578 for fixing listCommands.
     */
    virtual AllowedOnSecondary secondaryAllowed(ServiceContext* context) const = 0;

    /**
     * Override and return fales if the command opcounters should not be incremented on
     * behalf of this command.
     */
    virtual bool shouldAffectCommandCounter() const {
        return true;
    }

    /**
     * Override and return true if the readConcernCounters in serverStatus should not be incremented
     * on behalf of this command.
     */
    virtual bool shouldAffectReadConcernCounter() const {
        return false;
    }

    /**
      Returns true if this command collects operation resource consumption metrics.
     */
    virtual bool collectsResourceConsumptionMetrics() const {
        return false;
    }

    /**
     * Return true if the command requires auth.
     */
    virtual bool requiresAuth() const {
        return true;
    }

    /**
     * Generates help text for this command.
     */
    virtual std::string help() const {
        return "no help defined";
    }

    /**
     * Redacts "cmdObj" in-place to a form suitable for writing to logs.
     *
     * The default implementation removes the field returned by sensitiveFieldName.
     *
     * This is NOT used to implement user-configurable redaction of PII. Instead, that is
     * implemented via the set of redact() free functions, which are no-ops when log redaction is
     * disabled. All PII must pass through one of the redact() overloads before being logged.
     */
    virtual void snipForLogging(mutablebson::Document* cmdObj) const;

    /**
     * Marks a field name in a cmdObj as sensitive.
     *
     * The default snipForLogging shall remove these field names. Auditing shall not
     * include these fields in audit outputs.
     */
    virtual std::set<StringData> sensitiveFieldNames() const {
        return {};
    }

    /**
     * Return true if a replica set secondary should go into "recovering"
     * (unreadable) state while running this command.
     */
    virtual bool maintenanceMode() const {
        return false;
    }

    /**
     * Return true if command should be permitted when a replica set secondary is in "recovering"
     * (unreadable) state.
     */
    virtual bool maintenanceOk() const {
        return true; /* assumed true prior to commit */
    }

    /**
     * Returns LogicalOp for this command.
     */
    virtual LogicalOp getLogicalOp() const {
        return LogicalOp::opCommand;
    }

    /**
     * Returns whether this operation is a read, write, command, or multi-document transaction.
     *
     * Commands which implement database read or write logic should override this to return kRead
     * or kWrite as appropriate.
     */
    enum class ReadWriteType { kCommand, kRead, kWrite, kTransaction };
    virtual ReadWriteType getReadWriteType() const {
        return ReadWriteType::kCommand;
    }

    /**
     * Increment counter for how many times this command has executed.
     */
    void incrementCommandsExecuted() const {
        _commandsExecuted.increment();
    }

    /**
     * Increment counter for how many times this command has failed.
     */
    void incrementCommandsFailed() const {
        _commandsFailed.increment();
    }

    /**
     * Generates a reply from the 'help' information associated with a command. The state of
     * the passed ReplyBuilder will be in kOutputDocs after calling this method.
     */
    static void generateHelpResponse(OperationContext* opCtx,
                                     rpc::ReplyBuilderInterface* replyBuilder,
                                     const Command& command);

    /**
     * If true, the logical sessions attached to the command request will be attached to the
     * request's operation context. Note that returning false can potentially strip the logical
     * session from the request in multi-staged invocations, like for example, mongos -> mongod.
     * This can have security implications so think carefully before returning false.
     */
    virtual bool attachLogicalSessionsToOpCtx() const {
        return true;
    }

    /**
     * Checks if the command is also known by the provided alias.
     */
    bool hasAlias(const StringData& alias) const;

    /**
     * Audit when this command fails authz check.
     */
    virtual bool auditAuthorizationFailure() const {
        return true;
    }

    /**
     * By default, no newly created command is permitted under multitenancy.
     * Implementations must override this to true to permit use.
     */
    virtual bool allowedWithSecurityToken() const {
        return false;
    }

    /**
     * Get the authorization contract for this command. nullptr means no contract has been
     * specified.
     */
    virtual const AuthorizationContract* getAuthorizationContract() const {
        return nullptr;
    }

    /**
     * Returns true if this command supports apply once semantic when retried.
     */
    virtual bool supportsRetryableWrite() const {
        return false;
    }

    /**
     * Returns true if sessions should be checked out when lsid and txnNumber is present in the
     * request.
     */
    virtual bool shouldCheckoutSession() const {
        return true;
    }

    /**
     * Returns true if this is a command related to managing the lifecycle of a transaction.
     */
    virtual bool isTransactionCommand() const {
        return false;
    }

    /**
     * Returns true if this command can be run in a transaction.
     */
    virtual bool allowedInTransactions() const {
        return false;
    }

private:
    // The full name of the command
    const std::string _name;

    // The list of aliases for the command
    const std::vector<StringData> _aliases;

    // Counters for how many times this command has been executed and failed
    CounterMetric _commandsExecuted;
    CounterMetric _commandsFailed;
};

/**
 * Represents a single invocation of a given command.
 */
class CommandInvocation {
public:
    CommandInvocation(const Command* definition) : _definition(definition) {}

    CommandInvocation(const CommandInvocation&) = delete;
    CommandInvocation& operator=(const CommandInvocation&) = delete;

    virtual ~CommandInvocation();

    static void set(OperationContext* opCtx, std::shared_ptr<CommandInvocation> invocation);
    static std::shared_ptr<CommandInvocation> get(OperationContext* opCtx);

    /**
     * Runs the command, filling in result. Any exception thrown from here will cause result
     * to be reset and filled in with the error. Non-const to permit modifying the request
     * type to perform normalization. Calls that return normally without setting an "ok"
     * field into result are assumed to have completed successfully. Failure should be
     * indicated either by throwing (preferred), or by calling
     * `CommandHelpers::extractOrAppendOk`.
     */
    virtual void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) = 0;

    /**
     * Returns a future that can schedule asynchronous execution of the command. By default, the
     * future falls back to the execution of `run(...)`, thus the default semantics of
     * `runAsync(...)` is identical to that of `run(...).
     */
    virtual Future<void> runAsync(std::shared_ptr<RequestExecutionContext> rec) {
        run(rec->getOpCtx(), rec->getReplyBuilder());
        return Status::OK();
    }

    virtual void explain(OperationContext* opCtx,
                         ExplainOptions::Verbosity verbosity,
                         rpc::ReplyBuilderInterface* result) {
        uasserted(ErrorCodes::IllegalOperation,
                  str::stream() << "Cannot explain cmd: " << definition()->getName());
    }

    /**
     * The primary namespace on which this command operates. May just be the db.
     */
    virtual NamespaceString ns() const = 0;

    /**
     * Returns true if this command should be parsed for a writeConcern field and wait
     * for that write concern to be satisfied after the command runs.
     */
    virtual bool supportsWriteConcern() const = 0;

    /**
     * Returns this invocation's support for readConcern.
     */
    virtual ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                         bool isImplicitDefault) const {
        static const Status kReadConcernNotSupported{ErrorCodes::InvalidOptions,
                                                     "read concern not supported"};
        static const Status kDefaultReadConcernNotPermitted{ErrorCodes::InvalidOptions,
                                                            "default read concern not permitted"};
        return {{level != repl::ReadConcernLevel::kLocalReadConcern, kReadConcernNotSupported},
                {kDefaultReadConcernNotPermitted}};
    }

    /**
     * Returns if this invocation is safe to run on a borrowed threading model.
     *
     * In practice, this is attempting to predict if the operation will do network or storage reads
     * and writes. It will allow auth commands for the most part, since while they do involve
     * network or storage operations, they are not targeting the storage engine or remote
     * mongo-server nodes.
     */
    virtual bool isSafeForBorrowedThreads() const {
        if (definition()->maintenanceMode() || !definition()->maintenanceOk()) {
            // If the command has maintenance implications, it has storage implications.
            return false;
        }

        if (supportsWriteConcern()) {
            // If the command supports write concern, it has storage and network implications.
            return false;
        }

        if (auto result = supportsReadConcern(repl::ReadConcernLevel::kMajorityReadConcern,
                                              false /* isImplicitDefault */);
            result.readConcernSupport.isOK()) {
            // If the command supports read concern, it has storage and newtork implications.
            return false;
        }

        return true;
    }

    /**
     * Return if this invocation can be mirrored to secondaries
     */
    virtual bool supportsReadMirroring() const {
        return false;
    }

    /**
     * Return a BSONObj that can be safely mirrored to secondaries for cache warming
     */
    virtual void appendMirrorableRequest(BSONObjBuilder*) const {
        MONGO_UNREACHABLE;
    }

    /**
     * Returns if this invocation is a mirrored read.
     */
    bool isMirrored() const {
        return _mirrored;
    }

    /**
     * Sets that this operation is a mirrored read.
     */
    void markMirrored() {
        _mirrored = true;
    }

    /**
     * Returns true if command allows afterClusterTime in its readConcern. The command may not allow
     * it if it is specifically intended not to take any LockManager locks. Waiting for
     * afterClusterTime takes the MODE_IS lock.
     */
    virtual bool allowsAfterClusterTime() const {
        return true;
    }

    /**
     * Returns true if a command may be able to safely ignore prepare conflicts. Only commands that
     * can guarantee they will only perform reads may ignore prepare conflicts.
     */
    virtual bool canIgnorePrepareConflicts() const {
        return false;
    }

    /**
     * Returns true if this command invocation is allowed to utilize "speculative" majority reads to
     * service 'majority' read concern requests. This allows a query to satisfy a 'majority' read
     * without storage engine support for reading from a historical snapshot.
     *
     * Note: This feature is currently only limited to a very small subset of commands (related to
     * change streams), and is not intended to be generally used, which is why it is disabled by
     * default.
     */
    virtual bool allowsSpeculativeMajorityReads() const {
        return false;
    }

    /**
     * The command definition that this invocation runs.
     * Note: nonvirtual.
     */
    const Command* definition() const {
        return _definition;
    }

    /**
     * Throws DBException, most likely `ErrorCodes::Unauthorized`, unless
     * the client executing "opCtx" is authorized to run the given command
     * with the given parameters on the given named database.
     * Note: nonvirtual.
     * The 'request' must outlive this CommandInvocation.
     */
    void checkAuthorization(OperationContext* opCtx, const OpMsgRequest& request) const;

protected:
    ResourcePattern resourcePattern() const;

private:
    /**
     * Polymorphic extension point for `checkAuthorization`.
     * Throws unless `opCtx`'s client is authorized to `run()` this.
     */
    virtual void doCheckAuthorization(OperationContext* opCtx) const = 0;

    const Command* const _definition;

    bool _mirrored = false;
};

/**
 * A subclass of Command that only cares about the BSONObj body and doesn't need access to document
 * sequences. Commands should implement this class if they require access to the
 * ReplyBuilderInterface (e.g. to set the next invocation for an exhaust command).
 */
class BasicCommandWithReplyBuilderInterface : public Command {
private:
    class Invocation;

public:
    using Command::Command;

    virtual NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const {
        return CommandHelpers::parseNsFromCommand(dbName, cmdObj);
    }

    ResourcePattern parseResourcePattern(const DatabaseName& dbName, const BSONObj& cmdObj) const {
        return CommandHelpers::resourcePatternForNamespace(parseNs(dbName, cmdObj).ns());
    }

    //
    // Interface for subclasses to implement
    //

    /**
     * Runs the given command. Returns true upon success.
     */
    virtual bool runWithReplyBuilder(OperationContext* opCtx,
                                     const DatabaseName& dbName,
                                     const BSONObj& cmdObj,
                                     rpc::ReplyBuilderInterface* replyBuilder) = 0;

    /**
     * Provides a future that may run the command asynchronously. By default, it falls back to
     * runWithReplyBuilder.
     */
    virtual Future<void> runAsync(std::shared_ptr<RequestExecutionContext> rec,
                                  const DatabaseName& dbName) {
        if (!runWithReplyBuilder(
                rec->getOpCtx(), dbName, rec->getRequest().body, rec->getReplyBuilder()))
            return Status(ErrorCodes::FailedToRunWithReplyBuilder,
                          fmt::format("Failed to run command: {}", rec->getCommand()->getName()));
        return Status::OK();
    }

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
     */
    virtual Status explain(OperationContext* opCtx,
                           const OpMsgRequest& request,
                           ExplainOptions::Verbosity verbosity,
                           rpc::ReplyBuilderInterface* result) const;

    /**
     * Checks if the client associated with the given OperationContext is authorized to run this
     * command.
     * Command imlpementations MUST provide a method here, even if no authz checks are required.
     * Such commands should return Status::OK(), with a comment stating "No auth required".
     */
    virtual Status checkAuthForOperation(OperationContext* opCtx,
                                         const DatabaseName& dbName,
                                         const BSONObj& cmdObj) const = 0;

    /**
     * supportsWriteConcern returns true if this command should be parsed for a writeConcern
     * field and wait for that write concern to be satisfied after the command runs.
     *
     * @param cmd is a BSONObj representation of the command that is used to determine if the
     *            the command supports a write concern.
     */
    virtual bool supportsWriteConcern(const BSONObj& cmdObj) const = 0;

    /**
     * Returns true if this Command supports the given readConcern level. Takes the command object
     * and the name of the database on which it was invoked as arguments, so that readConcern can be
     * conditionally rejected based on the command's parameters and/or namespace.
     *
     * If a readConcern level argument is sent to a command that returns false the command processor
     * will reject the command, returning an appropriate error message.
     *
     * Note that this is never called on mongos. Sharded commands are responsible for forwarding
     * the option to the shards as needed. We rely on the shards to fail the commands in the
     * cases where it isn't supported.
     */
    virtual ReadConcernSupportResult supportsReadConcern(const BSONObj& cmdObj,
                                                         repl::ReadConcernLevel level,
                                                         bool isImplicitDefault) const {
        static const Status kReadConcernNotSupported{ErrorCodes::InvalidOptions,
                                                     "read concern not supported"};
        static const Status kDefaultReadConcernNotPermitted{
            ErrorCodes::InvalidOptions, "cluster wide default read concern not permitted"};
        return {{level != repl::ReadConcernLevel::kLocalReadConcern, kReadConcernNotSupported},
                {kDefaultReadConcernNotPermitted}};
    }

    /**
     * Return if the cmdObj can be mirrored to secondaries in some form
     */
    virtual bool supportsReadMirroring(const BSONObj& cmdObj) const {
        return false;
    }

    /**
     * Return a modified form of cmdObj that can be safely mirrored to secondaries for cache warming
     */
    virtual void appendMirrorableRequest(BSONObjBuilder*, const BSONObj&) const {
        MONGO_UNREACHABLE;
    }

    virtual bool allowsAfterClusterTime(const BSONObj& cmdObj) const {
        return true;
    }

    /**
     * Returns true if a command may be able to safely ignore prepare conflicts. Only commands that
     * can guarantee they will only perform reads may ignore prepare conflicts.
     */
    virtual bool canIgnorePrepareConflicts() const {
        return false;
    }

private:
    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& request) final;
};

/**
 * Commands should implement this class if they do not require access to the ReplyBuilderInterface.
 */
class BasicCommand : public BasicCommandWithReplyBuilderInterface {
public:
    using BasicCommandWithReplyBuilderInterface::BasicCommandWithReplyBuilderInterface;

    /**
     * Runs the given command. Returns true upon success.
     */
    virtual bool run(OperationContext* opCtx,
                     const DatabaseName& dbName,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) = 0;

    bool runWithReplyBuilder(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             const BSONObj& cmdObj,
                             rpc::ReplyBuilderInterface* replyBuilder) override {
        auto result = replyBuilder->getBodyBuilder();
        return run(opCtx, dbName, cmdObj, result);
    }
};

namespace {
// Used in BasicCommandWithRequestParser below.
template <typename T, typename = int>
struct CommandAlias {
    // An empty alias is equivalent to no alias, see CommandRegistry::registerCommand.
    static constexpr StringData kAlias = ""_sd;
};

template <typename T>
struct CommandAlias<T, decltype((void)T::kCommandAlias, 0)> {
    static constexpr StringData kAlias = T::kCommandAlias;
};

template <typename T>
constexpr StringData command_alias_v = CommandAlias<T>::kAlias;
}  // namespace

/**
 * A CRTP base class for BasicCommandWithRequestParser, which simplifies writing commands that
 * accept requests generated by IDL to enforce API versioning and to overcome the complexity
 * to standardize the output generated by commands using IDL.
 *
 * Derive from it as follows:
 *
 *     class MyCommand : public BasicCommandWithRequestParser<MyCommand> {...};
 *
 * The 'Derived' type parameter must have:
 *
 *   - 'Request' naming a usable request type.
 *     A usable Request type must have:
 *
 *      - a static member factory function 'parse', callable as:
 *
 *         const IDLParserContext& idlCtx = ...;
 *         const OpMsgRequest& opMsgRequest = ...;
 *         Request r = Request::parse(idlCtx, opMsgRequest);
 *
 *      which enables it to be parsed as an IDL command.
 *
 *      - a 'static constexpr StringData kCommandName' member.
 *      - (optional) a 'static constexpr StringData kCommandAlias' member.
 *
 *   - validateResult: that has a custom logic to validate the result BSON object
 *     to enforce API versioning.
 *
 */
template <typename Derived>
class BasicCommandWithRequestParser : public BasicCommandWithReplyBuilderInterface {
protected:
    BasicCommandWithRequestParser()
        : BasicCommandWithReplyBuilderInterface(Derived::Request::kCommandName,
                                                command_alias_v<typename Derived::Request>) {}

    BasicCommandWithRequestParser(StringData name) : BasicCommandWithReplyBuilderInterface(name) {}

    bool runWithReplyBuilder(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             const BSONObj& cmdObj,
                             rpc::ReplyBuilderInterface* replyBuilder) final {
        auto result = replyBuilder->getBodyBuilder();

        // To enforce API versioning
        auto requestParser = RequestParser(opCtx, dbName, cmdObj);

        auto cmdDone = runWithRequestParser(opCtx, dbName, cmdObj, requestParser, result);

        // Only validate results in test mode so that we don't expose users to errors if we
        // construct an invalid reply.
        if (getTestCommandsEnabled()) {
            validateResult(result.asTempObj());
        }

        return cmdDone;
    }

    class RequestParser;

    /**
     * Runs the given command. Returns true upon success.
     */
    virtual bool runWithRequestParser(OperationContext* opCtx,
                                      const DatabaseName& dbName,
                                      const BSONObj& cmdObj,
                                      const RequestParser& requestParser,
                                      BSONObjBuilder& result) = 0;

    // Custom logic to validate results to enforce API versioning.
    virtual void validateResult(const BSONObj& resultObj) = 0;

    /*
     * If the result is an error, assert that it satisfies the IDL-defined requirements on a
     * command error reply.
     * Calls to this function should be done only in test mode so that we don't expose users to
     * errors if we construct an invalid error reply.
     */
    static bool checkIsErrorStatus(const BSONObj& resultObj, const IDLParserContext& ctx) {
        auto wcStatus = getWriteConcernStatusFromCommandResult(resultObj);
        if (!wcStatus.isOK()) {
            if (wcStatus.code() == ErrorCodes::TypeMismatch) {
                // Result has "writeConcernError" field but it is not valid wce object.
                uassertStatusOK(wcStatus);
            }
        }

        if (resultObj.hasField("ok")) {
            auto status = getStatusFromCommandResult(resultObj);
            if (!status.isOK()) {
                // Will throw if the result doesn't match the ErrorReply.
                ErrorReply::parse(IDLParserContext("ErrorType", &ctx), resultObj);
                return true;
            }
        }

        return false;
    }
};

template <typename Derived>
class BasicCommandWithRequestParser<Derived>::RequestParser {
public:
    using RequestType = typename Derived::Request;

    RequestParser(OperationContext* opCtx, const DatabaseName& dbName, const BSONObj& cmdObj)
        : _request{_parseRequest(opCtx, dbName, cmdObj)} {}

    const RequestType& request() const {
        return _request;
    }

private:
    static RequestType _parseRequest(OperationContext* opCtx,
                                     const DatabaseName& dbName,
                                     const BSONObj& cmdObj) {
        return RequestType::parse(
            IDLParserContext(RequestType::kCommandName,
                             APIParameters::get(opCtx).getAPIStrict().value_or(false),
                             dbName.tenantId()),
            cmdObj);
    }

    RequestType _request;
};

/**
 * Deprecated. Do not add new subclasses.
 */
class ErrmsgCommandDeprecated : public BasicCommand {
    using BasicCommand::BasicCommand;
    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final;

    virtual bool errmsgRun(OperationContext* opCtx,
                           const std::string& db,
                           const BSONObj& cmdObj,
                           std::string& errmsg,
                           BSONObjBuilder& result) = 0;
};

/**
 * A CRTP base class for typed commands, which simplifies writing commands that
 * accept requests generated by IDL. Derive from it as follows:
 *
 *     class MyCommand : public TypedCommand<MyCommand> {...};
 *
 * The 'Derived' type parameter must have:
 *
 *   - 'Request' naming a usable request type.
 *     A usable Request type must have:
 *
 *      - a static member factory function 'parse', callable as:
 *
 *         const IDLParserContext& idlCtx = ...;
 *         const OpMsgRequest& opMsgRequest = ...;
 *         Request r = Request::parse(idlCtx, opMsgRequest);
 *
 *      which enables it to be parsed as an IDL command.
 *
 *      - a 'constexpr StringData kCommandName' member.
 *
 *     Any type generated by the "commands:" section in the IDL syntax meets these
 *     requirements.  Note that IDL "structs:" will not. This is the recommended way to
 *     provide this Derived::Request type rather than writing it by hand.
 *
 *   - 'Invocation' - names a type derived from either of the nested invocation
 *     base classes provided: InvocationBase or MinimalInvocationBase.
 */
template <typename Derived>
class TypedCommand : public Command {
public:
    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& opMsgRequest) final;

protected:
    class InvocationBase;

    // Used instead of InvocationBase when a command must customize the 'run()' member.
    class MinimalInvocationBase;

    // Commands that only have a single name don't need to define any constructors.
    TypedCommand() : TypedCommand(Derived::Request::kCommandName) {}
    explicit TypedCommand(StringData name) : TypedCommand(name, {}) {}
    TypedCommand(StringData name, StringData altName) : Command(name, altName) {}

private:
    class InvocationBaseInternal;
};

template <typename Derived>
class TypedCommand<Derived>::InvocationBaseInternal : public CommandInvocation {
public:
    using RequestType = typename Derived::Request;

    InvocationBaseInternal(OperationContext* opCtx,
                           const Command* command,
                           const OpMsgRequest& opMsgRequest)
        : CommandInvocation(command),
          _request{_parseRequest(opCtx, command, opMsgRequest)},
          _opMsgRequest{opMsgRequest} {}

protected:
    const RequestType& request() const {
        return _request;
    }

    const OpMsgRequest& unparsedRequest() const {
        return _opMsgRequest;
    }

private:
    static RequestType _parseRequest(OperationContext* opCtx,
                                     const Command* command,
                                     const OpMsgRequest& opMsgRequest) {

        bool apiStrict = APIParameters::get(opCtx).getAPIStrict().value_or(false);

        // A command with 'apiStrict' cannot be invoked with alias.
        if (opMsgRequest.getCommandName() != command->getName() && apiStrict) {
            uasserted(ErrorCodes::APIStrictError,
                      str::stream() << "Command invocation with name '"
                                    << opMsgRequest.getCommandName().toString()
                                    << "' is not allowed in 'apiStrict' mode, use '"
                                    << command->getName() << "' instead");
        }

        return RequestType::parse(IDLParserContext(command->getName(), apiStrict), opMsgRequest);
    }

    RequestType _request;

    const OpMsgRequest _opMsgRequest;
};

template <typename Derived>
class TypedCommand<Derived>::MinimalInvocationBase : public InvocationBaseInternal {
    // Implemented as just a strong typedef for InvocationBaseInternal.
    using InvocationBaseInternal::InvocationBaseInternal;
};

/*
 * Classes derived from TypedCommand::InvocationBase must:
 *
 *   - inherit constructors with 'using InvocationBase::InvocationBase;'.
 *
 *   - define a 'typedRun' method like:
 *
 *       R typedRun(OperationContext* opCtx);
 *
 *     where R is one of:
 *        - void
 *        - T, where T is usable with fillFrom.
 *
 *     Note: a void typedRun produces a "pass-fail" command. If it runs to completion
 *     the result will be considered and formatted as an "ok".
 *
 *  If the TypedCommand's Request type was specified with the IDL attribute:
 *
 *     namespace: concatenate_with_db
 *
 *  then the ns() method of its Invocation class method should be:
 *
 *     NamespaceString ns() const override {
 *         return request.getNamespace();
 *     }
 */
template <typename Derived>
class TypedCommand<Derived>::InvocationBase : public InvocationBaseInternal {
public:
    using InvocationBaseInternal::InvocationBaseInternal;

private:
    using Invocation = typename Derived::Invocation;

    /**
     * _callTypedRun and _runImpl implement the tagged dispatch from 'run'.
     */
    decltype(auto) _callTypedRun(OperationContext* opCtx) {
        return static_cast<Invocation*>(this)->typedRun(opCtx);
    }
    void _runImpl(std::true_type, OperationContext* opCtx, rpc::ReplyBuilderInterface*) {
        _callTypedRun(opCtx);
    }
    void _runImpl(std::false_type, OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) {
        reply->fillFrom(_callTypedRun(opCtx));
    }

    void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) final {
        using VoidResultTag = std::is_void<decltype(_callTypedRun(opCtx))>;
        _runImpl(VoidResultTag{}, opCtx, reply);
    }
};

template <typename Derived>
std::unique_ptr<CommandInvocation> TypedCommand<Derived>::parse(OperationContext* opCtx,
                                                                const OpMsgRequest& opMsgRequest) {
    return std::make_unique<typename Derived::Invocation>(opCtx, this, opMsgRequest);
}


/**
 * See the 'globalCommandRegistry()' singleton accessor.
 */
class CommandRegistry {
public:
    using CommandMap = Command::CommandMap;

    CommandRegistry() = default;
    CommandRegistry(const CommandRegistry&) = delete;
    CommandRegistry& operator=(const CommandRegistry&) = delete;

    const CommandMap& allCommands() const {
        return _commands;
    }

    void registerCommand(Command* command, StringData name, std::vector<StringData> aliases);

    Command* findCommand(StringData name) const;

    void incrementUnknownCommands() {
        _unknowns.increment();
    }

private:
    CounterMetric _unknowns{"commands.<UNKNOWN>"};
    CommandMap _commands;
};

/**
 * Accessor to the command registry, an always-valid singleton.
 */
CommandRegistry* globalCommandRegistry();

/**
 * Creates a test command object of type CmdType if test commands are enabled
 * for this process. Prefer this syntax to using MONGO_INITIALIZER directly.
 * The created Command object is "leaked" intentionally, since it will register
 * itself.
 *
 * The command objects will be created after the "default" initializer, and all
 * startup option processing happens prior to "default" (see base/init.h).
 */
#define MONGO_REGISTER_TEST_COMMAND(CmdType)                                \
    MONGO_INITIALIZER(RegisterTestCommand_##CmdType)(InitializerContext*) { \
        if (getTestCommandsEnabled()) {                                     \
            new CmdType();                                                  \
        }                                                                   \
    }

/**
 * Creates a command object of type CmdType if the featureFlag is enabled for
 * this process, regardless of the current FCV. Prefer this syntax to using
 * MONGO_INITIALIZER directly. The created Command object is "leaked"
 * intentionally, since it will register itself.
 *
 * The command objects will be created after the "default" initializer, and all
 * startup option processing happens prior to "default" (see base/init.h).
 */
#define MONGO_REGISTER_FEATURE_FLAGGED_COMMAND(CmdType, featureFlag)        \
    MONGO_INITIALIZER(RegisterTestCommand_##CmdType)(InitializerContext*) { \
        if (featureFlag.isEnabledAndIgnoreFCVUnsafeAtStartup()) {           \
            new CmdType();                                                  \
        }                                                                   \
    }

}  // namespace mongo
