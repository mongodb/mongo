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

#include <boost/optional.hpp>
#include <string>
#include <vector>

#include "mongo/base/counter.h"
#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/write_concern.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/string_map.h"

namespace mongo {

class Command;
class CommandInvocation;
class OperationContext;

namespace mutablebson {
class Document;
}  // namespace mutablebson

// Various helpers unrelated to any single command or to the command registry.
// Would be a namespace, but want to keep it closed rather than open.
// Some of these may move to the BasicCommand shim if they are only for legacy implementations.
struct CommandHelpers {
    // The type of the first field in 'cmdObj' must be mongo::String. The first field is
    // interpreted as a collection name.
    static std::string parseNsFullyQualified(const BSONObj& cmdObj);

    // The type of the first field in 'cmdObj' must be mongo::String or Symbol.
    // The first field is interpreted as a collection name.
    static NamespaceString parseNsCollectionRequired(StringData dbname, const BSONObj& cmdObj);

    static NamespaceStringOrUUID parseNsOrUUID(StringData dbname, const BSONObj& cmdObj);

    /**
     * Return the namespace for the command. If the first field in 'cmdObj' is of type
     * mongo::String, then that field is interpreted as the collection name, and is
     * appended to 'dbname' after a '.' character. If the first field is not of type
     * mongo::String, then 'dbname' is returned unmodified.
     */
    static std::string parseNsFromCommand(StringData dbname, const BSONObj& cmdObj);

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
     * Appends passthrough fields from a cmdObj to a given request.
     */
    static BSONObj appendPassthroughFields(const BSONObj& cmdObjWithPassthroughFields,
                                           const BSONObj& request);

    /**
     * Returns a copy of 'cmdObj' with a majority writeConcern appended.
     */
    static BSONObj appendMajorityWriteConcern(const BSONObj& cmdObj);

    /**
     * Returns true if the provided argument is one that is handled by the command processing layer
     * and should generally be ignored by individual command implementations. In particular,
     * commands that fail on unrecognized arguments must not fail for any of these.
     */
    static bool isGenericArgument(StringData arg) {
        // Not including "help" since we don't pass help requests through to the command parser.
        // If that changes, it should be added. When you add to this list, consider whether you
        // should also change the filterCommandRequestForPassthrough() function.
        return arg == "$audit" ||                        //
            arg == "$client" ||                          //
            arg == "$configServerState" ||               //
            arg == "$db" ||                              //
            arg == "allowImplicitCollectionCreation" ||  //
            arg == "$oplogQueryData" ||                  //
            arg == "$queryOptions" ||                    //
            arg == "$readPreference" ||                  //
            arg == "$replData" ||                        //
            arg == "$clusterTime" ||                     //
            arg == "maxTimeMS" ||                        //
            arg == "readConcern" ||                      //
            arg == "databaseVersion" ||                  //
            arg == "shardVersion" ||                     //
            arg == "tracking_info" ||                    //
            arg == "writeConcern" ||                     //
            arg == "lsid" ||                             //
            arg == "txnNumber" ||                        //
            arg == "autocommit" ||                       //
            arg == "startTransaction" ||                 //
            false;  // These comments tell clang-format to keep this line-oriented.
    }

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

    static void auditLogAuthEvent(OperationContext* opCtx,
                                  const CommandInvocation* invocation,
                                  const OpMsgRequest& request,
                                  ErrorCodes::Error err);
    /**
     * Overload taking a Command instead of CommandInvocation. It has to punt on the logged
     * namespace, giving only the request's $db. Since the Command hasn't parsed the request body,
     * we can't know the collection part of that namespace, so we leave it blank in the audit log.
     */
    static void auditLogAuthEvent(OperationContext* opCtx,
                                  const Command* command,
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

    static constexpr StringData kHelpFieldName = "help"_sd;
};

/**
 * Serves as a base for server commands. See the constructor for more details.
 */
class Command {
public:
    using CommandMap = StringMap<Command*>;
    enum class AllowedOnSecondary { kAlways, kNever, kOptIn };

    /**
     * Constructs a new command and causes it to be registered with the global commands list. It is
     * not safe to construct commands other than when the server is starting up.
     *
     * @param oldName an optional old, deprecated name for the command
     */
    Command(StringData name, StringData oldName = StringData());

    // Do not remove or relocate the definition of this "key function".
    // See https://gcc.gnu.org/wiki/VerboseDiagnostics#missing_vtable
    virtual ~Command();

    virtual std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                                     const OpMsgRequest& request) = 0;

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
     * Return true for "user management commands", a distinction that affects
     * backward compatible output formatting.
     */
    virtual bool isUserManagementCommand() const {
        return false;
    }

    /**
     * Return true if only the admin ns has privileges to run this command.
     */
    virtual bool adminOnly() const {
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
     * one place (i.e. 'listCommands') that inappropriately produces the "slaveOk" and
     * "slaveOverrideOk" fields for each Command without regard to payload. This is
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
     * The default implementation does nothing.
     *
     * This is NOT used to implement user-configurable redaction of PII. Instead, that is
     * implemented via the set of redact() free functions, which are no-ops when log redaction is
     * disabled. All PII must pass through one of the redact() overloads before being logged.
     */
    virtual void redactForLogging(mutablebson::Document* cmdObj) const {}

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
     * Returns whether this operation is a read, write, or command.
     *
     * Commands which implement database read or write logic should override this to return kRead
     * or kWrite as appropriate.
     */
    enum class ReadWriteType { kCommand, kRead, kWrite };
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

private:
    // The full name of the command
    const std::string _name;

    // Counters for how many times this command has been executed and failed
    mutable Counter64 _commandsExecuted;
    mutable Counter64 _commandsFailed;
    // Pointers to hold the metrics tree references
    ServerStatusMetricField<Counter64> _commandsExecutedMetric;
    ServerStatusMetricField<Counter64> _commandsFailedMetric;
};

class CommandReplyBuilder {
public:
    explicit CommandReplyBuilder(BSONObjBuilder bodyObj);

    CommandReplyBuilder(const CommandReplyBuilder&) = delete;
    CommandReplyBuilder& operator=(const CommandReplyBuilder&) = delete;

    /**
     * Returns a BSONObjBuilder that can be used to build the reply in-place. The returned
     * builder (or an object into which it has been moved) must be completed before calling
     * any more methods on this object. A builder is completed by a call to `done()` or by
     * its destructor. Can be called repeatedly to append multiple things to the reply, as
     * long as each returned builder must be completed between calls.
     */
    BSONObjBuilder getBodyBuilder() const;

    void reset();

    /**
     * Appends a key:object field to this reply.
     */
    template <typename T>
    void append(StringData key, const T& object) {
        getBodyBuilder() << key << object;
    }

    /**
     * The specified 'object' must be BSON-serializable.
     *
     * BSONSerializable 'x' means 'x.serialize(bob)' appends a representation of 'x'
     * into 'BSONObjBuilder* bob'.
     */
    template <typename T>
    void fillFrom(const T& object) {
        static_assert(!isStatusOrStatusWith<std::decay_t<T>>,
                      "Status and StatusWith<T> aren't supported by TypedCommand and fillFrom(). "
                      "Use uassertStatusOK() instead.");
        auto bob = getBodyBuilder();
        object.serialize(&bob);
    }

private:
    BufBuilder* const _bodyBuf;
    const std::size_t _bodyOffset;
};

/**
 * Represents a single invocation of a given command.
 */
class CommandInvocation {
public:
    CommandInvocation(const Command* definition) : _definition(definition) {}
    virtual ~CommandInvocation();

    /**
     * Runs the command, filling in result. Any exception thrown from here will cause result
     * to be reset and filled in with the error. Non-const to permit modifying the request
     * type to perform normalization. Calls that return normally without setting an "ok"
     * field into result are assumed to have completed successfully. Failure should be
     * indicated either by throwing (preferred), or by calling
     * `CommandHelpers::extractOrAppendOk`.
     */
    virtual void run(OperationContext* opCtx, CommandReplyBuilder* result) = 0;

    virtual void explain(OperationContext* opCtx,
                         ExplainOptions::Verbosity verbosity,
                         BSONObjBuilder* result) {
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
    virtual bool supportsReadConcern(repl::ReadConcernLevel level) const {
        return level == repl::ReadConcernLevel::kLocalReadConcern;
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
};

/**
 * A subclass of Command that only cares about the BSONObj body and doesn't need access to document
 * sequences.
 */
class BasicCommand : public Command {
private:
    class Invocation;

public:
    using Command::Command;

    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
        return CommandHelpers::parseNsFromCommand(dbname, cmdObj);
    }

    ResourcePattern parseResourcePattern(const std::string& dbname, const BSONObj& cmdObj) const {
        return CommandHelpers::resourcePatternForNamespace(parseNs(dbname, cmdObj));
    }

    //
    // Interface for subclasses to implement
    //

    /**
     * run the given command
     * implement this...
     *
     * return value is true if succeeded.  if false, set errmsg text.
     */
    virtual bool run(OperationContext* opCtx,
                     const std::string& db,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) = 0;

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
                           BSONObjBuilder* out) const;

    /**
     * Checks if the client associated with the given OperationContext is authorized to run this
     * command. Default implementation defers to checkAuthForCommand.
     */
    virtual Status checkAuthForOperation(OperationContext* opCtx,
                                         const std::string& dbname,
                                         const BSONObj& cmdObj) const;

    /**
     * supportsWriteConcern returns true if this command should be parsed for a writeConcern
     * field and wait for that write concern to be satisfied after the command runs.
     *
     * @param cmd is a BSONObj representation of the command that is used to determine if the
     *            the command supports a write concern. Ex. aggregate only supports write concern
     *            when $out is provided.
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
    virtual bool supportsReadConcern(const std::string& dbName,
                                     const BSONObj& cmdObj,
                                     repl::ReadConcernLevel level) const {
        return level == repl::ReadConcernLevel::kLocalReadConcern;
    }

    virtual bool allowsAfterClusterTime(const BSONObj& cmdObj) const {
        return true;
    }

private:
    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& request) final;

    //
    // Deprecated virtual methods.
    //

    /**
     * Checks if the given client is authorized to run this command on database "dbname"
     * with the invocation described by "cmdObj".
     *
     * NOTE: Implement checkAuthForOperation that takes an OperationContext* instead.
     */
    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const;

    /**
     * Appends to "*out" the privileges required to run this command on database "dbname" with
     * the invocation described by "cmdObj".  New commands shouldn't implement this, they should
     * implement checkAuthForOperation (which takes an OperationContext*), instead.
     */
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        // The default implementation of addRequiredPrivileges should never be hit.
        fassertFailed(16940);
    }
};

/**
 * Deprecated. Do not add new subclasses.
 */
class ErrmsgCommandDeprecated : public BasicCommand {
    using BasicCommand::BasicCommand;
    bool run(OperationContext* opCtx,
             const std::string& db,
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
 * The 'Derived' type paramter must have:
 *
 *   - 'Request' naming a usable request type.
 *     A usable Request type must have:
 *
 *      - a static member factory function 'parse', callable as:
 *
 *         const IDLParserErrorContext& idlCtx = ...;
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

    InvocationBaseInternal(OperationContext*,
                           const Command* command,
                           const OpMsgRequest& opMsgRequest)
        : CommandInvocation(command), _request{_parseRequest(command->getName(), opMsgRequest)} {}

protected:
    const RequestType& request() const {
        return _request;
    }

private:
    static RequestType _parseRequest(StringData name, const OpMsgRequest& opMsgRequest) {
        return RequestType::parse(IDLParserErrorContext(name), opMsgRequest);
    }

    RequestType _request;
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
    void _runImpl(std::true_type, OperationContext* opCtx, CommandReplyBuilder*) {
        _callTypedRun(opCtx);
    }
    void _runImpl(std::false_type, OperationContext* opCtx, CommandReplyBuilder* reply) {
        reply->fillFrom(_callTypedRun(opCtx));
    }

    void run(OperationContext* opCtx, CommandReplyBuilder* reply) final {
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

    CommandRegistry() : _unknownsMetricField("commands.<UNKNOWN>", &_unknowns) {}

    CommandRegistry(const CommandRegistry&) = delete;
    CommandRegistry& operator=(const CommandRegistry&) = delete;

    const CommandMap& allCommands() const {
        return _commands;
    }

    void registerCommand(Command* command, StringData name, StringData oldName);

    Command* findCommand(StringData name) const;

    void incrementUnknownCommands() {
        _unknowns.increment();
    }

private:
    Counter64 _unknowns;
    ServerStatusMetricField<Counter64> _unknownsMetricField;

    CommandMap _commands;
};

/**
 * Accessor to the command registry, an always-valid singleton.
 */
CommandRegistry* globalCommandRegistry();

/**
 * Creates a test command object of type CmdType if test commands are enabled for this process.
 * Prefer this syntax to using MONGO_INITIALIZER directly.
 * The created Command object is "leaked" intentionally, since it will register itself.
 */
#define MONGO_REGISTER_TEST_COMMAND(CmdType)                                \
    MONGO_INITIALIZER(RegisterTestCommand_##CmdType)(InitializerContext*) { \
        if (getTestCommandsEnabled()) {                                     \
            new CmdType();                                                  \
        }                                                                   \
        return Status::OK();                                                \
    }

}  // namespace mongo
