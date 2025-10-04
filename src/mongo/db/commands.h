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

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/explain_verbosity_gen.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/request_execution_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/platform/source_location.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/get_status_from_command_result_write_util.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/transport/service_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/modules_incompletely_marked_header.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/optional.hpp>
#include <fmt/format.h>

namespace mongo {

class AuthorizationContract;
class Command;
class CommandInvocation;
class EncryptionInformation;
class OperationContext;

namespace mutablebson {
class Document;
}  // namespace mutablebson

extern FailPoint failCommand;
extern FailPoint waitInCommandMarkKillOnClientDisconnect;

extern const std::set<std::string> kNoApiVersions;
extern const std::set<std::string> kApiVersions1;

boost::optional<BSONArray>& errorLabelsOverride(OperationContext* opCtx);

/**
 * Whether or not the caller should rewrite the request for FLE. If this function returns false,
 * no FLE rewriting is needed.
 * A side effect of calling this function is that diagnostics can be disabled in the passed
 * OperationContext.
 */
bool prepareForFLERewrite(OperationContext* opCtx,
                          const boost::optional<EncryptionInformation>& encryptionInformation);

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
    virtual void onBeforeRun(OperationContext* opCtx, CommandInvocation* invocation) = 0;

    /**
     * A behavior to perform before CommandInvocation::asyncRun(). Defaults to `onBeforeRun(...)`.
     */
    virtual void onBeforeAsyncRun(std::shared_ptr<RequestExecutionContext> rec,
                                  CommandInvocation* invocation) {
        onBeforeRun(rec->getOpCtx(), invocation);
    }

    /**
     * A behavior to perform after CommandInvocation::run(). Note that the response argument is not
     * const, because the ReplyBuilderInterface does not expose any const methods to inspect the
     * response body. However, onAfterRun must not mutate the response body.
     */
    virtual void onAfterRun(OperationContext* opCtx,
                            CommandInvocation* invocation,
                            rpc::ReplyBuilderInterface* response) = 0;

    /**
     * A behavior to perform after CommandInvocation::asyncRun(). Defaults to `onAfterRun(...)`.
     */
    virtual void onAfterAsyncRun(std::shared_ptr<RequestExecutionContext> rec,
                                 CommandInvocation* invocation) {
        onAfterRun(rec->getOpCtx(), invocation, rec->getReplyBuilder());
    }
};

// Various helpers unrelated to any single command or to the command registry.
// Would be a namespace, but want to keep it closed rather than open.
// Some of these may move to the BasicCommand shim if they are only for legacy implementations.
struct CommandHelpers {
    // The type of the first field in 'cmdObj' must be BSONType::string. The first field is
    // interpreted as a collection name.
    static std::string parseNsFullyQualified(const BSONObj& cmdObj);

    // The type of the first field in 'cmdObj' must be BSONType::string or Symbol.
    // The first field is interpreted as a collection name.
    static NamespaceString parseNsCollectionRequired(const DatabaseName& dbName,
                                                     const BSONObj& cmdObj);

    static NamespaceStringOrUUID parseNsOrUUID(const DatabaseName& dbName, const BSONObj& cmdObj);

    // Check that the namespace string references a collection that holds documents
    // rather than an internal configuration collection (the names of which contain
    // a $). The one exception is kLocalOplogDollarMain, which is considered valid
    // despite containing $.
    static void ensureValidCollectionName(const NamespaceString& nss);

    /**
     * Return the namespace for the command. If the first field in 'cmdObj' is of type
     * BSONType::string, then that field is interpreted as the collection name.
     * If the first field is not of type BSONType::string, then the namespace only has database
     * name.
     */
    static NamespaceString parseNsFromCommand(const DatabaseName& dbName, const BSONObj& cmdObj);

    /**
     * Utility that returns a ResourcePattern for the namespace returned from
     * BasicCommand::parseNs(dbname, cmdObj). This will be either an exact namespace resource
     * pattern or a database resource pattern, depending on whether parseNs returns a fully qualifed
     * collection name or just a database name.
     */
    static ResourcePattern resourcePatternForNamespace(const NamespaceString& ns);

    static Command* findCommand(Service* service, StringData name);
    static Command* findCommand(OperationContext* opCtx, StringData name);

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
     * Parses an "ok" field, or appends "ok" to the command reply. Additionally returns a Status
     * representing an "ok" or an error.
     */
    static Status extractOrAppendOkAndGetStatus(BSONObjBuilder& reply);

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
     *
     * Use generic_argument_util::setMajorityWriteConcern() instead if the BSON is generated from an
     * IDL-command struct.
     *
     * TODO SERVER-91373: Remove this function in favor of
     * generic_argument_util::setMajorityWriteConcern().
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
     * namespaces, and throws if that verification doesn't pass.
     */
    static void canUseTransactions(const std::vector<NamespaceString>& namespaces,
                                   Command* command,
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
 * Fast by-name comparision for Command.
 *
 * Outside of a Command instance, should generally be declared static,
 * as the constructor does a string lookup, which is slower than the
 * string compare we're trying to avoid.
 *
 * The integer indicies will be small, so we could expand the interface
 * to use this as an array index in the future.
 */
class CommandNameAtom {
public:
    explicit CommandNameAtom(StringData s);

    auto operator<=>(const CommandNameAtom&) const = default;
    bool operator==(const CommandNameAtom&) const = default;

private:
    size_t _atom;
};

/**
 * Serves as a base for server commands. See the constructor for more details.
 */
class MONGO_MOD_OPEN Command {
public:
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

    CommandNameAtom getNameAtom() const {
        return _atom;
    }

    /** Returns the command's aliases if any. Constant. */
    const std::vector<StringData>& getAliases() const {
        return _aliases;
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

    /**
     * After a Command is created, we tell it what Cluster `role` it has.
     * Role will be `ShardServer` or `RouterServer`, exclusively.
     * In tests, `role` might be `None` as there's no associated Service.
     * The virtual `doInitializeClsuterRole` is then invoked to allow
     * derived types to perform additional role-aware initialization.
     */
    void initializeClusterRole(ClusterRole role);

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
     * Override and return false if the command opcounters should not be incremented on
     * behalf of this command.
     */
    virtual bool shouldAffectCommandCounter() const {
        return true;
    }

    /**
     * Override and return true if the query opcounters should be incremented on
     * behalf of this command.
     */
    virtual bool shouldAffectQueryCounter() const {
        return false;
    }

    /**
     * Override and return true if the readConcernCounters and readPreferenceCounters in
     * serverStatus should be incremented on behalf of this command. This should be true for
     * read operations.
     */
    virtual bool shouldAffectReadOptionCounters() const {
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
     * Return true if this Command type is eligible for diagnostic printing when the command
     * unexpectedly fails due to a tassert, invariant, or signal such as segfault.
     */
    virtual bool enableDiagnosticPrintingOnFailure() const {
        return false;
    }

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
     *
     * `kLast` is only a marker to specify the number of entries in the list.
     */
    enum class ReadWriteType { kCommand, kRead, kWrite, kTransaction, kLast };
    virtual ReadWriteType getReadWriteType() const {
        return ReadWriteType::kCommand;
    }

    /**
     * Increment counter for how many times this command has executed.
     */
    void incrementCommandsExecuted() const {
        if (_commandsExecuted)
            _commandsExecuted->increment();
    }

    /**
     * Increment counter for how many times this command has failed.
     */
    void incrementCommandsFailed() const {
        if (_commandsFailed)
            _commandsFailed->increment();
    }

    /**
     * Increment counter for how many times this command has been rejected
     * due to query settings.
     */
    void incrementCommandsRejected() const {
        _commandsRejected->increment();
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
    bool hasAlias(StringData alias) const;

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

    /**
     * Override to true if this command should be allowed on a direct shard connection regardless
     * of the directShardOperations ActionType.
     */
    virtual bool shouldSkipDirectConnectionChecks() const {
        return false;
    }

    /**
     * Returns false if this command manually opts out of mandatory authorization checks, true
     otherwise. Will enable mandatory authorization checks by default.
     */
    virtual bool requiresAuthzChecks() const {
        return true;
    }

protected:
    /** For extended role-dependent initialization. */
    virtual void doInitializeClusterRole(ClusterRole role) {}

private:
    const std::string _name;
    const CommandNameAtom _atom{_name};
    const std::vector<StringData> _aliases;

    // Counters for how many times this command has been executed and failed
    Counter64* _commandsExecuted{};
    Counter64* _commandsFailed{};
    Counter64* _commandsRejected{};
};

/**
 * Represents a single invocation of a given command.
 */
class MONGO_MOD_OPEN CommandInvocation {
public:
    CommandInvocation(const Command* definition) : _definition(definition) {}

    CommandInvocation(const CommandInvocation&) = delete;
    CommandInvocation& operator=(const CommandInvocation&) = delete;

    virtual ~CommandInvocation();

    static void set(OperationContext* opCtx, std::shared_ptr<CommandInvocation> invocation);
    static std::shared_ptr<CommandInvocation>& get(OperationContext* opCtx);

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
     * The database associated with this command (i.e. the "$db" field in OP_MSG requests).
     *
     * This is usually equivalent to ns().dbName(), but some commands are associated with a
     * different database (usually admin) than the one they modify or operate over.
     */
    virtual const DatabaseName& db() const = 0;

    /**
     * All of the namespaces this command operates on. For most commands will just be ns().
     */
    virtual std::vector<NamespaceString> allNamespaces() const {
        return {ns()};
    }

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
     * Returns whether this invocation supports the rawData command parameter. See
     * raw_data_operation.h for more information.
     */
    virtual bool supportsRawData() const {
        return false;
    }

    /**
     * Returns if this invocation can be mirrored to secondaries
     */
    virtual bool supportsReadMirroring() const {
        return false;
    }

    /**
     * Returns the name of the database that should be targeted for the mirrored read.
     */
    virtual DatabaseName getDBForReadMirroring() const {
        return ns().dbName();
    }

    /**
     * Returns a BSONObj that can be safely mirrored to secondaries for cache warming.
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
     * Returns true if this command invocation should wait until there are ingress admission tickets
     * available before it is allowed to run.
     */
    virtual bool isSubjectToIngressAdmissionControl() const {
        return false;
    }

    virtual bool isReadOperation() const {
        return _definition->getReadWriteType() == Command::ReadWriteType::kRead;
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

    virtual const GenericArguments& getGenericArguments() const = 0;

    /**
     * Returns true when this command is safe to retry on a StaleConfig or
     * ShardCannotRefreshDueToLocksHeld error. Commands can override this method with their own
     * retry logic.
     */
    virtual bool canRetryOnStaleConfigOrShardCannotRefreshDueToLocksHeld(
        const OpMsgRequest& request) const {
        return true;
    }

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
class MONGO_MOD_OPEN BasicCommandWithReplyBuilderInterface : public Command {
private:
    class Invocation;

public:
    using Command::Command;

    virtual NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const {
        return CommandHelpers::parseNsFromCommand(dbName, cmdObj);
    }

    ResourcePattern parseResourcePattern(const DatabaseName& dbName, const BSONObj& cmdObj) const {
        return CommandHelpers::resourcePatternForNamespace(parseNs(dbName, cmdObj));
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
     * Command implementations MUST provide a method here, even if no authz checks are required.
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
     * Returns whether this command supports the rawData parameter. See raw_data_operation.h for
     * more information.
     */
    virtual bool supportsRawData() const {
        return false;
    }

    /**
     * Returns if the cmdObj can be mirrored to secondaries in some form.
     */
    virtual bool supportsReadMirroring(const BSONObj& cmdObj) const {
        return false;
    }

    /**
     * Returns a modified form of cmdObj that can be safely mirrored to secondaries for cache
     * warming.
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

    /**
     * Returns true if this command should wait until there are ingress admission tickets
     * available before it is allowed to run.
     */
    virtual bool isSubjectToIngressAdmissionControl() const {
        return false;
    }

private:
    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& request) final;
};

/**
 * Commands should implement this class if they do not require access to the ReplyBuilderInterface.
 */
class MONGO_MOD_OPEN BasicCommand : public BasicCommandWithReplyBuilderInterface {
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
class MONGO_MOD_OPEN BasicCommandWithRequestParser : public BasicCommandWithReplyBuilderInterface {
private:
    static constexpr StringData _commandAlias() {
        using T = typename Derived::Request;
        if constexpr (requires { T::kCommandAlias; }) {
            return T::kCommandAlias;
        } else {
            return {};  // Empty. Means no alias.
        }
    }

protected:
    BasicCommandWithRequestParser()
        : BasicCommandWithReplyBuilderInterface(Derived::Request::kCommandName, _commandAlias()) {}

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
                ErrorReply::parse(resultObj, IDLParserContext("ErrorType", &ctx));
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
        return idl::parseCommandDocument<RequestType>(
            cmdObj,
            IDLParserContext(RequestType::kCommandName,
                             auth::ValidatedTenancyScope::get(opCtx),
                             dbName.tenantId(),
                             SerializationContext::stateDefault()));
    }

    RequestType _request;
};

/**
 * Deprecated. Do not add new subclasses.
 */
class MONGO_MOD_OPEN ErrmsgCommandDeprecated : public BasicCommand {
    using BasicCommand::BasicCommand;
    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final;

    virtual bool errmsgRun(OperationContext* opCtx,
                           const DatabaseName& dbName,
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
class MONGO_MOD_OPEN TypedCommand : public Command {
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

    const DatabaseName& db() const override {
        return request().getDbName();
    }

protected:
    const RequestType& request() const {
        return _request;
    }

    RequestType& request() {
        return _request;
    }

    const OpMsgRequest& unparsedRequest() const {
        return _opMsgRequest;
    }

    const GenericArguments& getGenericArguments() const override {
        return request().getGenericArguments();
    }

private:
    static RequestType _parseRequest(OperationContext* opCtx,
                                     const Command* command,
                                     const OpMsgRequest& opMsgRequest) {
        auto ctx = IDLParserContext(command->getName(),
                                    opMsgRequest.validatedTenancyScope,
                                    opMsgRequest.getValidatedTenantId(),
                                    opMsgRequest.getSerializationContext());
        auto parsed = idl::parseCommandRequest<RequestType>(opMsgRequest, ctx);

        auto apiStrict = parsed.getGenericArguments().getApiStrict().value_or(false);

        // A command with 'apiStrict' cannot be invoked with alias.
        if (apiStrict && opMsgRequest.getCommandName() != command->getName()) {
            uasserted(ErrorCodes::APIStrictError,
                      str::stream()
                          << "Command invocation with name '" << opMsgRequest.getCommandName()
                          << "' is not allowed in 'apiStrict' mode, use '" << command->getName()
                          << "' instead");
        }

        return parsed;
    }

    RequestType _request;

    const OpMsgRequest _opMsgRequest;
};

template <typename Derived>
class MONGO_MOD_OPEN TypedCommand<Derived>::MinimalInvocationBase : public InvocationBaseInternal {
    // Implemented as just a strong typedef for InvocationBaseInternal.
    using InvocationBaseInternal::InvocationBaseInternal;
};

/**
 * Mix-in base for requests containing a `GenericArguments`.
 * Fills some of the requirements for use as a `TypedCommand`'s `Request` type.
 */
class GenericArgumentsTypedRequest {
public:
    explicit GenericArgumentsTypedRequest(const OpMsgRequest& req) : _args{_parseArgs(req)} {}

    const GenericArguments& getGenericArguments() const {
        return _args;
    }
    GenericArguments& getGenericArguments() {
        return _args;
    }
    void setGenericArguments(GenericArguments args) {
        _args = std::move(args);
    }

private:
    static GenericArguments _parseArgs(const OpMsgRequest& req) {
        IDLParserContext ctx("GenericArguments",
                             req.validatedTenancyScope,
                             req.getValidatedTenantId(),
                             req.getSerializationContext());
        return GenericArguments::parse(req.body, ctx);
    }

    GenericArguments _args;
};

/**
 * Mix-in base for Requests containing a DatabaseName.
 * Fills some of the requirements for use as a `TypedCommand`'s `Request` type.
 */
class DbNameTypedRequest {
public:
    explicit DbNameTypedRequest(const OpMsgRequest& req) : _dbName{req.parseDbName()} {}

    const DatabaseName& getDbName() const {
        return _dbName;
    }

private:
    DatabaseName _dbName;
};

/**
 * Base for Requests having a `GenericArguments` and a `DatabaseName`.
 * Fills the `TypedCommand` `Request` requirements.
 */
class BasicTypedRequest : public GenericArgumentsTypedRequest, public DbNameTypedRequest {
public:
    explicit BasicTypedRequest(const OpMsgRequest& req)
        : GenericArgumentsTypedRequest{req}, DbNameTypedRequest{req} {}
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
class MONGO_MOD_OPEN TypedCommand<Derived>::InvocationBase : public InvocationBaseInternal {
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


class CommandRegistry {
public:
    /**
     * Invokes a callable `f` for each distinct `Command* c` in the registry, as `f(c)`.
     * A `Command*` may be mapped to multiple aliases, but these are omitted
     * from the callback sequence.
     */
    template <typename F>
    void forEachCommand(const F& f) const {
        for (const auto& [command, entry] : _commands)
            f(entry->command);
    }

    /** Add `command` to the registry. */
    void registerCommand(Command* command);

    Command* findCommand(StringData name) const;

    void incrementUnknownCommands() {
        if (_onUnknown)
            _onUnknown();
    }

    void logWeakRegistrations() const;

    /** A production `CommandRegistry` will update a counter. */
    std::function<void()> setOnUnknownCommandCallback(std::function<void()> cb) {
        return std::exchange(_onUnknown, std::move(cb));
    }

private:
    struct Entry {
        Command* command;
    };

    stdx::unordered_map<Command*, std::unique_ptr<Entry>> _commands;
    StringMap<Command*> _commandNames;
    std::function<void()> _onUnknown;
};

CommandRegistry* getCommandRegistry(Service* service);

/** Convenience overload. */
inline CommandRegistry* getCommandRegistry(OperationContext* opCtx) {
    return getCommandRegistry(opCtx->getService());
}

inline Command* CommandHelpers::findCommand(Service* service, StringData name) {
    return getCommandRegistry(service)->findCommand(name);
}

inline Command* CommandHelpers::findCommand(OperationContext* opCtx, StringData name) {
    return getCommandRegistry(opCtx)->findCommand(name);
}

/**
 * When CommandRegistry objects are initialized, they look into the global
 * CommandConstructionPlan to find the list of Command objects that need to
 * be created.
 *
 * It will be populated mainly by the use of EntryBuilder objects.
 */
class CommandConstructionPlan {
public:
    struct Entry {
        std::function<std::unique_ptr<Command>()> construct;
        FeatureFlag* featureFlag = nullptr;
        bool testOnly = false;
        boost::optional<ClusterRole> roles;
        const std::type_info* typeInfo = nullptr;
        boost::optional<SourceLocation> location;
        std::string expr;
    };

    class EntryBuilder;

    void addEntry(std::unique_ptr<Entry> e) {
        _entries.push_back(std::move(e));
    }

    const std::vector<std::unique_ptr<Entry>>& entries() const {
        return _entries;
    }

    /**
     * Adds to the specified `registry` an instance of all apppriate Command types in this plan.
     * Appropriate is determined by the Entry data members, and by the specified `pred`.
     *
     * There are some server-wide criteria applied automatically:
     *
     *   - FeatureFlag-enabled commands are filtered out according to flag settings.
     *
     *   - testOnly registrations are only created if the server is in testOnly mode.
     *
     * Other criteria can be applied via the caller-supplied `pred`. A `Command`
     * will only be created for an `entry` if the `pred(entry)` passes.
     */
    void execute(CommandRegistry* registry,
                 Service* service,
                 const std::function<bool(const Entry&)>& pred) const;

    /**
     * Calls `execute` with a predicate that enables Commands appropriate for
     * the specified `service`.
     */
    void execute(CommandRegistry* registry, Service* service) const;

private:
    std::vector<std::unique_ptr<Entry>> _entries;
};

BSONObj toBSON(const CommandConstructionPlan::Entry& e);

/**
 * CommandRegisterer objects attach entries to this instance at static-init
 * time by default.
 */
CommandConstructionPlan& globalCommandConstructionPlan();

/**
 * Builder type designed to inject entries into the global
 * `CommandConstructionPlan`.
 * Example:
 *
 *   auto dum = *CommandConstructionPlan::EntryBuilder::make<CmdType>()
 *       .requiresFeatureFlag(myFeatureFlag)
 *       .testOnly();
 */
class CommandConstructionPlan::EntryBuilder {
public:
    /**
     * Returns a builder specifying a simple value-initialized instance of
     * `Cmd`.
     */
    template <typename Cmd>
    static EntryBuilder make() {
        EntryBuilder eb;
        eb._entry->construct = [] {
            return std::unique_ptr<Command>{std::make_unique<Cmd>()};
        };
        eb._entry->typeInfo = &typeid(Cmd);
        return eb;
    }

    EntryBuilder() = default;

    /**
     * Role specification is mandatory for all EntryBuilders, through addRoles,
     * forShard, and forRouter.
     */
    EntryBuilder addRoles(ClusterRole role) && {
        _entry->roles = _entry->roles.value_or(ClusterRole::None);
        for (auto&& r : {ClusterRole::ShardServer, ClusterRole::RouterServer})
            if (role.has(r))
                *_entry->roles += r;
        return std::move(*this);
    }

    /** Add the shard server role. */
    EntryBuilder forShard() && {
        return std::move(*this).addRoles(ClusterRole::ShardServer);
    }

    /** Add the router server role. */
    EntryBuilder forRouter() && {
        return std::move(*this).addRoles(ClusterRole::RouterServer);
    }

    /**
     * Denotes a test-only command. See docs/test_commands.md.
     */
    EntryBuilder testOnly() && {
        _entry->testOnly = true;
        return std::move(*this);
    }

    /**
     * A command object will be created only if the featureFlag is enabled,
     * regardless of the current FCV.
     */
    EntryBuilder requiresFeatureFlag(FeatureFlag& featureFlag) && {
        _entry->featureFlag = &featureFlag;
        return std::move(*this);
    }

    /**
     * Set the plan into which the entry will be registered. Used for testing.
     * The default is the `globalCommandConstructionPlan()` singleton.
     */
    EntryBuilder setPlan_forTest(CommandConstructionPlan* plan) && {
        _plan = plan;
        return std::move(*this);
    }

    EntryBuilder location(SourceLocation loc) && {
        _entry->location = loc;
        return std::move(*this);
    }

    EntryBuilder expr(std::string name) && {
        _entry->expr = std::move(name);
        return std::move(*this);
    }

    /** The deref operator executes the build, registering the product. */
    EntryBuilder operator*() && {
        _plan->addEntry(std::move(_entry));
        return std::move(*this);
    }

private:
    CommandConstructionPlan* _plan = &globalCommandConstructionPlan();
    std::unique_ptr<Entry> _entry = std::make_unique<Entry>();
};

#define MONGO_COMMAND_DUMMY_ID_INNER_(x, y) x##y
#define MONGO_COMMAND_DUMMY_ID_(x, y) MONGO_COMMAND_DUMMY_ID_INNER_(x, y)

/**
 * Creates a builder for CommandConstuctorPlan entry, which will
 * create a Command of the specified CmdType.
 *
 * Does not end with `;`, which allows attachment of
 * properties to the command plan entry before it is registered.
 * Example:
 *
 *     MONGO_REGISTER_COMMAND(MyCommandType)
 *        .testOnly()
 *        .forFeatureFlag(&myFeatureFlag)
 *        .forShard();
 */
#define MONGO_REGISTER_COMMAND(...)                                              \
    static auto MONGO_COMMAND_DUMMY_ID_(mongoRegisterCommand_dummy_, __LINE__) = \
        *::mongo::CommandConstructionPlan::EntryBuilder::make<__VA_ARGS__>()     \
             .expr(#__VA_ARGS__)                                                 \
             .location(MONGO_SOURCE_LOCATION())

}  // namespace mongo
