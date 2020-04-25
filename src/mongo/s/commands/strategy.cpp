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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/commands/strategy.h"

#include <fmt/format.h>

#include "mongo/base/data_cursor.h"
#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/curop.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/initialize_operation_session_info.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_time_tracker.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/logical_time_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_find.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/shard_invalidated_for_targeting_exception.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/transport/ismaster_metrics.h"
#include "mongo/transport/session.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

using namespace fmt::literals;

namespace mongo {
namespace {

const auto kOperationTime = "operationTime"_sd;

/**
 * Extract and process metadata from the command request body.
 */
Status processCommandMetadata(OperationContext* opCtx, const BSONObj& cmdObj) {
    ReadPreferenceSetting::get(opCtx) =
        uassertStatusOK(ReadPreferenceSetting::fromContainingBSON(cmdObj));

    auto logicalClock = LogicalClock::get(opCtx);
    invariant(logicalClock);

    auto logicalTimeMetadata = rpc::LogicalTimeMetadata::readFromMetadata(cmdObj);
    if (!logicalTimeMetadata.isOK()) {
        return logicalTimeMetadata.getStatus();
    }

    auto logicalTimeValidator = LogicalTimeValidator::get(opCtx);
    const auto& signedTime = logicalTimeMetadata.getValue().getSignedTime();

    // No need to check proof is no time is given.
    if (signedTime.getTime() == LogicalTime::kUninitialized) {
        return Status::OK();
    }

    if (!LogicalTimeValidator::isAuthorizedToAdvanceClock(opCtx)) {
        auto advanceClockStatus = logicalTimeValidator->validate(opCtx, signedTime);

        if (!advanceClockStatus.isOK()) {
            return advanceClockStatus;
        }
    }

    return logicalClock->advanceClusterTime(signedTime.getTime());
}

/**
 * Append required fields to command response.
 */
void appendRequiredFieldsToResponse(OperationContext* opCtx, BSONObjBuilder* responseBuilder) {
    auto validator = LogicalTimeValidator::get(opCtx);
    if (validator->shouldGossipLogicalTime()) {
        auto now = LogicalClock::get(opCtx)->getClusterTime();

        // Add operationTime.
        auto operationTime = OperationTimeTracker::get(opCtx)->getMaxOperationTime();
        if (operationTime != LogicalTime::kUninitialized) {
            LOGV2_DEBUG(22764,
                        5,
                        "Appending operationTime: {operationTime}",
                        "Appending operationTime",
                        "operationTime"_attr = operationTime.asTimestamp());
            responseBuilder->append(kOperationTime, operationTime.asTimestamp());
        } else if (now != LogicalTime::kUninitialized) {
            // If we don't know the actual operation time, use the cluster time instead. This is
            // safe but not optimal because we can always return a later operation time than actual.
            LOGV2_DEBUG(22765,
                        5,
                        "Appending clusterTime as operationTime {clusterTime}",
                        "Appending clusterTime as operationTime",
                        "clusterTime"_attr = now.asTimestamp());
            responseBuilder->append(kOperationTime, now.asTimestamp());
        }

        // Add $clusterTime.
        if (LogicalTimeValidator::isAuthorizedToAdvanceClock(opCtx)) {
            SignedLogicalTime dummySignedTime(now, TimeProofService::TimeProof(), 0);
            rpc::LogicalTimeMetadata(dummySignedTime).writeToMetadata(responseBuilder);
        } else {
            auto currentTime = validator->signLogicalTime(opCtx, now);
            rpc::LogicalTimeMetadata(currentTime).writeToMetadata(responseBuilder);
        }
    }
}

/**
 * Invokes the given command and aborts the transaction on any non-retryable errors.
 */
void invokeInTransactionRouter(OperationContext* opCtx,
                               const OpMsgRequest& request,
                               CommandInvocation* invocation,
                               rpc::ReplyBuilderInterface* result) {
    auto txnRouter = TransactionRouter::get(opCtx);
    invariant(txnRouter);

    // No-op if the transaction is not running with snapshot read concern.
    txnRouter.setDefaultAtClusterTime(opCtx);

    try {
        CommandHelpers::runCommandInvocation(opCtx, request, invocation, result);
    } catch (const DBException& e) {
        if (ErrorCodes::isSnapshotError(e.code()) ||
            ErrorCodes::isNeedRetargettingError(e.code()) ||
            e.code() == ErrorCodes::ShardInvalidatedForTargeting ||
            e.code() == ErrorCodes::StaleDbVersion) {
            // Don't abort on possibly retryable errors.
            throw;
        }

        txnRouter.implicitlyAbortTransaction(opCtx, e.toStatus());
        throw;
    }
}

/**
 * Adds info from the active transaction and the given reason as context to the active exception.
 */
void addContextForTransactionAbortingError(StringData txnIdAsString,
                                           StmtId latestStmtId,
                                           DBException& ex,
                                           StringData reason) {
    ex.addContext(str::stream() << "Transaction " << txnIdAsString << " was aborted on statement "
                                << latestStmtId << " due to: " << reason);
}

void execCommandClient(OperationContext* opCtx,
                       CommandInvocation* invocation,
                       const OpMsgRequest& request,
                       rpc::ReplyBuilderInterface* result) {
    const Command* c = invocation->definition();
    ON_BLOCK_EXIT([opCtx, &result] {
        auto body = result->getBodyBuilder();
        appendRequiredFieldsToResponse(opCtx, &body);
    });

    const auto dbname = request.getDatabase();
    uassert(ErrorCodes::IllegalOperation,
            "Can't use 'local' database through mongos",
            dbname != NamespaceString::kLocalDb);
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid database name: '" << dbname << "'",
            NamespaceString::validDBName(dbname, NamespaceString::DollarInDbNameBehavior::Allow));

    StringMap<int> topLevelFields;
    for (auto&& element : request.body) {
        StringData fieldName = element.fieldNameStringData();
        if (fieldName == "help" && element.type() == Bool && element.Bool()) {
            std::stringstream help;
            help << "help for: " << c->getName() << " " << c->help();
            auto body = result->getBodyBuilder();
            body.append("help", help.str());
            CommandHelpers::appendSimpleCommandStatus(body, true, "");
            return;
        }

        uassert(ErrorCodes::FailedToParse,
                str::stream() << "Parsed command object contains duplicate top level key: "
                              << fieldName,
                topLevelFields[fieldName]++ == 0);
    }

    try {
        invocation->checkAuthorization(opCtx, request);
    } catch (const DBException& e) {
        auto body = result->getBodyBuilder();
        CommandHelpers::appendCommandStatusNoThrow(body, e.toStatus());
        return;
    }

    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
        uassert(ErrorCodes::InvalidOptions,
                "read concern snapshot is only supported in a multi-statement transaction",
                TransactionRouter::get(opCtx));
    }

    // attach tracking
    rpc::TrackingMetadata trackingMetadata;
    trackingMetadata.initWithOperName(c->getName());
    rpc::TrackingMetadata::get(opCtx) = trackingMetadata;

    auto metadataStatus = processCommandMetadata(opCtx, request.body);
    if (!metadataStatus.isOK()) {
        auto body = result->getBodyBuilder();
        CommandHelpers::appendCommandStatusNoThrow(body, metadataStatus);
        return;
    }

    auto txnRouter = TransactionRouter::get(opCtx);
    if (txnRouter) {
        invokeInTransactionRouter(opCtx, request, invocation, result);
    } else {
        CommandHelpers::runCommandInvocation(opCtx, request, invocation, result);
    }

    if (invocation->supportsWriteConcern()) {
        failCommand.executeIf(
            [&](const BSONObj& data) {
                result->getBodyBuilder().append(data["writeConcernError"]);
                if (data.hasField(kErrorLabelsFieldName) &&
                    data[kErrorLabelsFieldName].type() == Array) {
                    auto labels = data.getObjectField(kErrorLabelsFieldName).getOwned();
                    if (!labels.isEmpty()) {
                        result->getBodyBuilder().append(kErrorLabelsFieldName, BSONArray(labels));
                    }
                }
            },
            [&](const BSONObj& data) {
                return CommandHelpers::shouldActivateFailCommandFailPoint(
                           data, invocation, opCtx->getClient()) &&
                    data.hasField("writeConcernError");
            });
    }

    auto body = result->getBodyBuilder();

    bool ok = CommandHelpers::extractOrAppendOk(body);
    if (!ok) {
        c->incrementCommandsFailed();

        if (auto txnRouter = TransactionRouter::get(opCtx)) {
            txnRouter.implicitlyAbortTransaction(opCtx,
                                                 getStatusFromCommandResult(body.asTempObj()));
        }
    }
}

MONGO_FAIL_POINT_DEFINE(doNotRefreshShardsOnRetargettingError);

/**
 * Executes the command for the given request, and appends the result to replyBuilder
 * and error labels, if any, to errorBuilder.
 */
void runCommand(OperationContext* opCtx,
                const OpMsgRequest& request,
                const NetworkOp opType,
                rpc::ReplyBuilderInterface* replyBuilder,
                BSONObjBuilder* errorBuilder) {
    auto const commandName = request.getCommandName();
    auto const command = CommandHelpers::findCommand(commandName);
    if (!command) {
        auto builder = replyBuilder->getBodyBuilder();
        ON_BLOCK_EXIT([opCtx, &builder] { appendRequiredFieldsToResponse(opCtx, &builder); });
        CommandHelpers::appendCommandStatusNoThrow(
            builder,
            {ErrorCodes::CommandNotFound, str::stream() << "no such cmd: " << commandName});
        globalCommandRegistry()->incrementUnknownCommands();
        return;
    }

    CommandHelpers::uassertShouldAttemptParse(opCtx, command, request);

    // Parse the 'maxTimeMS' command option, and use it to set a deadline for the operation on
    // the OperationContext. Be sure to do this as soon as possible so that further processing by
    // subsequent code has the deadline available. The 'maxTimeMS' option unfortunately has a
    // different meaning for a getMore command, where it is used to communicate the maximum time to
    // wait for new inserts on tailable cursors, not as a deadline for the operation.
    // TODO SERVER-34277 Remove the special handling for maxTimeMS for getMores. This will
    // require introducing a new 'max await time' parameter for getMore, and eventually banning
    // maxTimeMS altogether on a getMore command.
    uassert(ErrorCodes::InvalidOptions,
            "no such command option $maxTimeMs; use maxTimeMS instead",
            request.body[QueryRequest::queryOptionMaxTimeMS].eoo());
    const int maxTimeMS = uassertStatusOK(
        QueryRequest::parseMaxTimeMS(request.body[QueryRequest::cmdOptionMaxTimeMS]));
    if (maxTimeMS > 0 && command->getLogicalOp() != LogicalOp::opGetMore) {
        opCtx->setDeadlineAfterNowBy(Milliseconds{maxTimeMS}, ErrorCodes::MaxTimeMSExpired);
    }
    opCtx->checkForInterrupt();  // May trigger maxTimeAlwaysTimeOut fail point.

    // If the command includes a 'comment' field, set it on the current OpCtx.
    if (auto commentField = request.body["comment"]) {
        opCtx->setComment(commentField.wrap());
    }

    std::shared_ptr<CommandInvocation> invocation = command->parse(opCtx, request);
    CommandInvocation::set(opCtx, invocation);

    // Set the logical optype, command object and namespace as soon as we identify the command. If
    // the command does not define a fully-qualified namespace, set CurOp to the generic command
    // namespace db.$cmd.
    std::string ns = invocation->ns().toString();
    auto nss = (request.getDatabase() == ns ? NamespaceString(ns, "$cmd") : NamespaceString(ns));

    // Fill out all currentOp details.
    CurOp::get(opCtx)->setGenericOpRequestDetails(opCtx, nss, command, request.body, opType);

    auto osi = initializeOperationSessionInfo(opCtx,
                                              request.body,
                                              command->requiresAuth(),
                                              command->attachLogicalSessionsToOpCtx(),
                                              true,
                                              true);

    // TODO SERVER-28756: Change allowTransactionsOnConfigDatabase to true once we fix the bug
    // where the mongos custom write path incorrectly drops the client's txnNumber.
    auto allowTransactionsOnConfigDatabase = false;
    validateSessionOptions(osi, command->getName(), nss, allowTransactionsOnConfigDatabase);

    auto wc = uassertStatusOK(WriteConcernOptions::extractWCFromCommand(request.body));

    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    auto readConcernParseStatus = [&]() {
        // We must obtain the client lock to set the ReadConcernArgs on the operation
        // context as it may be concurrently read by CurrentOp.
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        return readConcernArgs.initialize(request.body);
    }();
    if (!readConcernParseStatus.isOK()) {
        auto builder = replyBuilder->getBodyBuilder();
        CommandHelpers::appendCommandStatusNoThrow(builder, readConcernParseStatus);
        return;
    }

    if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
        uassert(ErrorCodes::InvalidOptions,
                "read concern snapshot is not supported with atClusterTime on mongos",
                !readConcernArgs.getArgsAtClusterTime());
    }

    boost::optional<RouterOperationContextSession> routerSession;
    try {
        rpc::readRequestMetadata(opCtx, request.body, command->requiresAuth());

        CommandHelpers::evaluateFailCommandFailPoint(opCtx, invocation.get());
        bool startTransaction = false;
        if (osi.getAutocommit()) {
            routerSession.emplace(opCtx);

            auto txnRouter = TransactionRouter::get(opCtx);
            invariant(txnRouter);

            auto txnNumber = opCtx->getTxnNumber();
            invariant(txnNumber);

            auto transactionAction = ([&] {
                auto startTxnSetting = osi.getStartTransaction();
                if (startTxnSetting && *startTxnSetting) {
                    return TransactionRouter::TransactionActions::kStart;
                }

                if (command->getName() == CommitTransaction::kCommandName) {
                    return TransactionRouter::TransactionActions::kCommit;
                }

                return TransactionRouter::TransactionActions::kContinue;
            })();

            startTransaction = (transactionAction == TransactionRouter::TransactionActions::kStart);
            txnRouter.beginOrContinueTxn(opCtx, *txnNumber, transactionAction);
        }

        bool supportsWriteConcern = invocation->supportsWriteConcern();
        if (!supportsWriteConcern &&
            request.body.hasField(WriteConcernOptions::kWriteConcernField)) {
            // This command doesn't do writes so it should not be passed a writeConcern.
            auto responseBuilder = replyBuilder->getBodyBuilder();
            CommandHelpers::appendCommandStatusNoThrow(
                responseBuilder,
                Status(ErrorCodes::InvalidOptions, "Command does not support writeConcern"));
            return;
        }

        bool clientSuppliedWriteConcern = !wc.usedDefault;
        bool customDefaultWriteConcernWasApplied = false;

        if (supportsWriteConcern && !clientSuppliedWriteConcern &&
            (!TransactionRouter::get(opCtx) || isTransactionCommand(commandName))) {
            // This command supports WC, but wasn't given one - so apply the default, if there is
            // one.
            if (const auto wcDefault = ReadWriteConcernDefaults::get(opCtx->getServiceContext())
                                           .getDefaultWriteConcern(opCtx)) {
                wc = *wcDefault;
                customDefaultWriteConcernWasApplied = true;
                LOGV2_DEBUG(22766,
                            2,
                            "Applying default writeConcern on {command} of {writeConcern}",
                            "Applying default writeConcern on command",
                            "command"_attr = request.getCommandName(),
                            "writeConcern"_attr = *wcDefault);
            }
        }

        if (TransactionRouter::get(opCtx)) {
            validateWriteConcernForTransaction(wc, commandName);
        }

        if (supportsWriteConcern) {
            auto& provenance = wc.getProvenance();

            // ClientSupplied is the only provenance that clients are allowed to pass to mongos.
            if (provenance.hasSource() && !provenance.isClientSupplied()) {
                auto responseBuilder = replyBuilder->getBodyBuilder();
                CommandHelpers::appendCommandStatusNoThrow(
                    responseBuilder,
                    Status{ErrorCodes::InvalidOptions,
                           "writeConcern provenance must be unset or \"{}\""_format(
                               ReadWriteConcernProvenance::kClientSupplied)});
                return;
            }

            // If the client didn't provide a provenance, then an appropriate value needs to be
            // determined.
            if (!provenance.hasSource()) {
                if (clientSuppliedWriteConcern) {
                    provenance.setSource(ReadWriteConcernProvenance::Source::clientSupplied);
                } else if (customDefaultWriteConcernWasApplied) {
                    provenance.setSource(ReadWriteConcernProvenance::Source::customDefault);
                } else {
                    provenance.setSource(ReadWriteConcernProvenance::Source::implicitDefault);
                }
            }

            // Ensure that the WC being set on the opCtx has provenance.
            invariant(wc.getProvenance().hasSource(),
                      str::stream()
                          << "unexpected unset provenance on writeConcern: " << wc.toBSON());

            opCtx->setWriteConcern(wc);
        }

        bool clientSuppliedReadConcern = readConcernArgs.isSpecified();
        bool customDefaultReadConcernWasApplied = false;

        auto readConcernSupport = invocation->supportsReadConcern(readConcernArgs.getLevel());
        if (readConcernSupport.defaultReadConcernPermit.isOK() &&
            (startTransaction || !TransactionRouter::get(opCtx))) {
            if (readConcernArgs.isEmpty()) {
                const auto rcDefault = ReadWriteConcernDefaults::get(opCtx->getServiceContext())
                                           .getDefaultReadConcern(opCtx);
                if (rcDefault) {
                    {
                        // We must obtain the client lock to set ReadConcernArgs, because it's an
                        // in-place reference to the object on the operation context, which may be
                        // concurrently used elsewhere (eg. read by currentOp).
                        stdx::lock_guard<Client> lk(*opCtx->getClient());
                        readConcernArgs = std::move(*rcDefault);
                    }
                    customDefaultReadConcernWasApplied = true;
                    LOGV2_DEBUG(22767,
                                2,
                                "Applying default readConcern on {command} of {readConcern}",
                                "Applying default readConcern on command",
                                "command"_attr = invocation->definition()->getName(),
                                "readConcern"_attr = *rcDefault);
                    // Update the readConcernSupport, since the default RC was applied.
                    readConcernSupport =
                        invocation->supportsReadConcern(readConcernArgs.getLevel());
                }
            }
        }

        auto& provenance = readConcernArgs.getProvenance();

        // ClientSupplied is the only provenance that clients are allowed to pass to mongos.
        if (provenance.hasSource() && !provenance.isClientSupplied()) {
            auto responseBuilder = replyBuilder->getBodyBuilder();
            CommandHelpers::appendCommandStatusNoThrow(
                responseBuilder,
                Status{ErrorCodes::InvalidOptions,
                       "readConcern provenance must be unset or \"{}\""_format(
                           ReadWriteConcernProvenance::kClientSupplied)});
            return;
        }

        // If the client didn't provide a provenance, then an appropriate value needs to be
        // determined.
        if (!provenance.hasSource()) {
            // We must obtain the client lock to set the provenance of the opCtx's ReadConcernArgs
            // as it may be concurrently read by CurrentOp.
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            if (clientSuppliedReadConcern) {
                provenance.setSource(ReadWriteConcernProvenance::Source::clientSupplied);
            } else if (customDefaultReadConcernWasApplied) {
                provenance.setSource(ReadWriteConcernProvenance::Source::customDefault);
            } else {
                provenance.setSource(ReadWriteConcernProvenance::Source::implicitDefault);
            }
        }

        // Ensure that the RC on the opCtx has provenance.
        invariant(readConcernArgs.getProvenance().hasSource(),
                  str::stream() << "unexpected unset provenance on readConcern: "
                                << readConcernArgs.toBSONInner());

        // If we are starting a transaction, we only need to check whether the read concern is
        // appropriate for running a transaction. There is no need to check whether the specific
        // command supports the read concern, because all commands that are allowed to run in a
        // transaction must support all applicable read concerns.
        if (startTransaction) {
            if (!isReadConcernLevelAllowedInTransaction(readConcernArgs.getLevel())) {
                auto responseBuilder = replyBuilder->getBodyBuilder();
                CommandHelpers::appendCommandStatusNoThrow(
                    responseBuilder,
                    {ErrorCodes::InvalidOptions,
                     "The readConcern level must be either 'local' (default), 'majority' or "
                     "'snapshot' in order to run in a transaction"});
                return;
            }
            if (readConcernArgs.getArgsOpTime()) {
                auto responseBuilder = replyBuilder->getBodyBuilder();
                CommandHelpers::appendCommandStatusNoThrow(
                    responseBuilder,
                    {ErrorCodes::InvalidOptions,
                     str::stream()
                         << "The readConcern cannot specify '"
                         << repl::ReadConcernArgs::kAfterOpTimeFieldName << "' in a transaction"});
                return;
            }
        }

        // Otherwise, if there is a read concern present - either user-specified or the default -
        // then check whether the command supports it. If there is no explicit read concern level,
        // then it is implicitly "local". There is no need to check whether this is supported,
        // because all commands either support "local" or upconvert the absent readConcern to a
        // stronger level that they do support; e.g. $changeStream upconverts to RC "majority".
        //
        // Individual transaction statements are checked later on, after we've unstashed the
        // transaction resources.
        if (!TransactionRouter::get(opCtx) && readConcernArgs.hasLevel()) {
            if (!readConcernSupport.readConcernSupport.isOK()) {
                auto responseBuilder = replyBuilder->getBodyBuilder();
                CommandHelpers::appendCommandStatusNoThrow(
                    responseBuilder,
                    readConcernSupport.readConcernSupport.withContext(
                        str::stream() << "Command " << invocation->definition()->getName()
                                      << " does not support " << readConcernArgs.toString()));
                return;
            }
        }

        // Remember whether or not this operation is starting a transaction, in case something later
        // in the execution needs to adjust its behavior based on this.
        opCtx->setIsStartingMultiDocumentTransaction(startTransaction);

        command->incrementCommandsExecuted();

        auto shouldAffectCommandCounter = command->shouldAffectCommandCounter();

        if (shouldAffectCommandCounter) {
            globalOpCounters.gotCommand();
        }

        ON_BLOCK_EXIT([opCtx, shouldAffectCommandCounter] {
            if (shouldAffectCommandCounter) {
                Grid::get(opCtx)->catalogCache()->checkAndRecordOperationBlockedByRefresh(
                    opCtx, mongo::LogicalOp::opCommand);
            }
        });


        for (int tries = 0;; ++tries) {
            // Try kMaxNumStaleVersionRetries times. On the last try, exceptions are rethrown.
            bool canRetry = tries < kMaxNumStaleVersionRetries - 1;

            if (tries > 0) {
                // Re-parse before retrying in case the process of run()-ning the
                // invocation could affect the parsed result.
                invocation = command->parse(opCtx, request);
                invariant(invocation->ns().toString() == ns,
                          "unexpected change of namespace when retrying");
            }

            replyBuilder->reset();
            try {
                execCommandClient(opCtx, invocation.get(), request, replyBuilder);

                auto responseBuilder = replyBuilder->getBodyBuilder();
                if (auto txnRouter = TransactionRouter::get(opCtx)) {
                    txnRouter.appendRecoveryToken(&responseBuilder);
                }

                return;
            } catch (ShardInvalidatedForTargetingException& ex) {
                auto catalogCache = Grid::get(opCtx)->catalogCache();
                catalogCache->setOperationShouldBlockBehindCatalogCacheRefresh(opCtx, true);

                // Retry logic specific to transactions. Throws and aborts the transaction if the
                // error cannot be retried on.
                if (auto txnRouter = TransactionRouter::get(opCtx)) {
                    auto abortGuard = makeGuard(
                        [&] { txnRouter.implicitlyAbortTransaction(opCtx, ex.toStatus()); });

                    if (!canRetry) {
                        addContextForTransactionAbortingError(txnRouter.txnIdToString(),
                                                              txnRouter.getLatestStmtId(),
                                                              ex,
                                                              "exhausted retries");
                        throw;
                    }

                    // TODO SERVER-39704 Allow mongos to retry on stale shard, stale db, snapshot,
                    // or shard invalidated for targeting errors.
                    if (!txnRouter.canContinueOnStaleShardOrDbError(commandName, ex.toStatus())) {
                        (void)catalogCache->getCollectionRoutingInfoWithRefresh(
                            opCtx, ex.extraInfo<ShardInvalidatedForTargetingInfo>()->getNss());
                        addContextForTransactionAbortingError(
                            txnRouter.txnIdToString(),
                            txnRouter.getLatestStmtId(),
                            ex,
                            "an error from cluster data placement change");
                        throw;
                    }

                    // The error is retryable, so update transaction state before retrying.
                    txnRouter.onStaleShardOrDbError(opCtx, commandName, ex.toStatus());

                    abortGuard.dismiss();
                    continue;
                }

                if (canRetry) {
                    continue;
                }
                throw;
            } catch (ExceptionForCat<ErrorCategory::NeedRetargettingError>& ex) {
                const auto staleNs = [&] {
                    if (auto staleInfo = ex.extraInfo<StaleConfigInfo>()) {
                        return staleInfo->getNss();
                    }
                    throw;
                }();

                auto catalogCache = Grid::get(opCtx)->catalogCache();
                if (auto staleInfo = ex.extraInfo<StaleConfigInfo>()) {
                    catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
                        opCtx,
                        staleNs,
                        staleInfo->getVersionWanted(),
                        staleInfo->getVersionReceived(),
                        staleInfo->getShardId());
                } else {
                    // If we don't have the stale config info and therefore don't know the shard's
                    // id, we have to force all further targetting requests for the namespace to
                    // block on a refresh.
                    catalogCache->onEpochChange(staleNs);
                }


                catalogCache->setOperationShouldBlockBehindCatalogCacheRefresh(opCtx, true);

                // Retry logic specific to transactions. Throws and aborts the transaction if the
                // error cannot be retried on.
                if (auto txnRouter = TransactionRouter::get(opCtx)) {
                    auto abortGuard = makeGuard(
                        [&] { txnRouter.implicitlyAbortTransaction(opCtx, ex.toStatus()); });

                    if (!canRetry) {
                        addContextForTransactionAbortingError(txnRouter.txnIdToString(),
                                                              txnRouter.getLatestStmtId(),
                                                              ex,
                                                              "exhausted retries");
                        throw;
                    }

                    // TODO SERVER-39704 Allow mongos to retry on stale shard, stale db, snapshot,
                    // or shard invalidated for targeting errors.
                    if (!txnRouter.canContinueOnStaleShardOrDbError(commandName, ex.toStatus())) {
                        addContextForTransactionAbortingError(
                            txnRouter.txnIdToString(),
                            txnRouter.getLatestStmtId(),
                            ex,
                            "an error from cluster data placement change");
                        throw;
                    }

                    // The error is retryable, so update transaction state before retrying.
                    txnRouter.onStaleShardOrDbError(opCtx, commandName, ex.toStatus());

                    abortGuard.dismiss();
                    continue;
                }

                if (canRetry) {
                    continue;
                }
                throw;
            } catch (ExceptionFor<ErrorCodes::StaleDbVersion>& ex) {
                // Mark database entry in cache as stale.
                Grid::get(opCtx)->catalogCache()->onStaleDatabaseVersion(ex->getDb(),
                                                                         ex->getVersionReceived());

                // Retry logic specific to transactions. Throws and aborts the transaction if the
                // error cannot be retried on.
                if (auto txnRouter = TransactionRouter::get(opCtx)) {
                    auto abortGuard = makeGuard(
                        [&] { txnRouter.implicitlyAbortTransaction(opCtx, ex.toStatus()); });

                    if (!canRetry) {
                        addContextForTransactionAbortingError(txnRouter.txnIdToString(),
                                                              txnRouter.getLatestStmtId(),
                                                              ex,
                                                              "exhausted retries");
                        throw;
                    }

                    // TODO SERVER-39704 Allow mongos to retry on stale shard, stale db, snapshot,
                    // or shard invalidated for targeting errors.
                    if (!txnRouter.canContinueOnStaleShardOrDbError(commandName, ex.toStatus())) {
                        addContextForTransactionAbortingError(
                            txnRouter.txnIdToString(),
                            txnRouter.getLatestStmtId(),
                            ex,
                            "an error from cluster data placement change");
                        throw;
                    }

                    // The error is retryable, so update transaction state before retrying.
                    txnRouter.onStaleShardOrDbError(opCtx, commandName, ex.toStatus());

                    abortGuard.dismiss();
                    continue;
                }

                if (canRetry) {
                    continue;
                }
                throw;
            } catch (ExceptionForCat<ErrorCategory::SnapshotError>& ex) {
                // Simple retry on any type of snapshot error.

                // Retry logic specific to transactions. Throws and aborts the transaction if the
                // error cannot be retried on.
                if (auto txnRouter = TransactionRouter::get(opCtx)) {
                    auto abortGuard = makeGuard(
                        [&] { txnRouter.implicitlyAbortTransaction(opCtx, ex.toStatus()); });

                    if (!canRetry) {
                        addContextForTransactionAbortingError(txnRouter.txnIdToString(),
                                                              txnRouter.getLatestStmtId(),
                                                              ex,
                                                              "exhausted retries");
                        throw;
                    }

                    // TODO SERVER-39704 Allow mongos to retry on stale shard, stale db, snapshot,
                    // or shard invalidated for targeting errors.
                    if (!txnRouter.canContinueOnSnapshotError()) {
                        addContextForTransactionAbortingError(txnRouter.txnIdToString(),
                                                              txnRouter.getLatestStmtId(),
                                                              ex,
                                                              "a non-retryable snapshot error");
                        throw;
                    }

                    // The error is retryable, so update transaction state before retrying.
                    txnRouter.onSnapshotError(opCtx, ex.toStatus());

                    abortGuard.dismiss();
                    continue;
                }

                if (canRetry) {
                    continue;
                }
                throw;
            }
            MONGO_UNREACHABLE;
        }
    } catch (const DBException& e) {
        command->incrementCommandsFailed();
        LastError::get(opCtx->getClient()).setLastError(e.code(), e.reason());
        // WriteConcern error (wcCode) is set to boost::none because:
        // 1. TransientTransaction error label handling for commitTransaction command in mongos is
        //    delegated to the shards. Mongos simply propagates the shard's response up to the
        //    client.
        // 2. For other commands in a transaction, they shouldn't get a writeConcern error so
        //    this setting doesn't apply.
        //
        // isInternalClient is set to true to suppress mongos from returning the RetryableWriteError
        // label.
        auto errorLabels = getErrorLabels(
            opCtx, osi, command->getName(), e.code(), boost::none, true /* isInternalClient */);
        errorBuilder->appendElements(errorLabels);
        throw;
    }
}

}  // namespace

DbResponse Strategy::queryOp(OperationContext* opCtx, const NamespaceString& nss, DbMessage* dbm) {
    globalOpCounters.gotQuery();

    ON_BLOCK_EXIT([opCtx] {
        Grid::get(opCtx)->catalogCache()->checkAndRecordOperationBlockedByRefresh(
            opCtx, mongo::LogicalOp::opQuery);
    });

    const QueryMessage q(*dbm);

    const auto upconvertedQuery = upconvertQueryEntry(q.query, nss, q.ntoreturn, q.ntoskip);

    // Set the upconverted query as the CurOp command object.
    CurOp::get(opCtx)->setGenericOpRequestDetails(
        opCtx, nss, nullptr, upconvertedQuery, dbm->msg().operation());

    Client* const client = opCtx->getClient();
    AuthorizationSession* const authSession = AuthorizationSession::get(client);

    // The legacy '$comment' operator gets converted to 'comment' by upconvertQueryEntry(). We
    // set the comment in 'opCtx' so that it can be passed on to the respective shards.
    if (auto commentField = upconvertedQuery["comment"]) {
        opCtx->setComment(commentField.wrap());
    }

    Status status = authSession->checkAuthForFind(nss, false);
    audit::logQueryAuthzCheck(client, nss, q.query, status.code());
    uassertStatusOK(status);

    LOGV2_DEBUG(22768,
                3,
                "Query: {namespace} {query} ntoreturn: {ntoreturn} options: {queryOptions}",
                "Query",
                "namespace"_attr = q.ns,
                "query"_attr = redact(q.query),
                "ntoreturn"_attr = q.ntoreturn,
                "queryOptions"_attr = q.queryOptions);

    if (q.queryOptions & QueryOption_Exhaust) {
        uasserted(18526,
                  str::stream() << "The 'exhaust' query option is invalid for mongos queries: "
                                << nss.ns() << " " << q.query.toString());
    }

    // Determine the default read preference mode based on the value of the slaveOk flag.
    const auto defaultReadPref = q.queryOptions & QueryOption_SlaveOk
        ? ReadPreference::SecondaryPreferred
        : ReadPreference::PrimaryOnly;
    ReadPreferenceSetting::get(opCtx) =
        uassertStatusOK(ReadPreferenceSetting::fromContainingBSON(q.query, defaultReadPref));

    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto canonicalQuery = uassertStatusOK(
        CanonicalQuery::canonicalize(opCtx,
                                     q,
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures));

    const QueryRequest& queryRequest = canonicalQuery->getQueryRequest();
    // Handle query option $maxTimeMS (not used with commands).
    if (queryRequest.getMaxTimeMS() > 0) {
        uassert(50749,
                "Illegal attempt to set operation deadline within DBDirectClient",
                !opCtx->getClient()->isInDirectClient());
        opCtx->setDeadlineAfterNowBy(Milliseconds{queryRequest.getMaxTimeMS()},
                                     ErrorCodes::MaxTimeMSExpired);
    }
    opCtx->checkForInterrupt();  // May trigger maxTimeAlwaysTimeOut fail point.

    // If the $explain flag was set, we must run the operation on the shards as an explain command
    // rather than a find command.
    if (queryRequest.isExplain()) {
        const BSONObj findCommand = queryRequest.asFindCommand();

        // We default to allPlansExecution verbosity.
        const auto verbosity = ExplainOptions::Verbosity::kExecAllPlans;

        BSONObjBuilder explainBuilder;
        Strategy::explainFind(opCtx,
                              findCommand,
                              queryRequest,
                              verbosity,
                              ReadPreferenceSetting::get(opCtx),
                              &explainBuilder);

        BSONObj explainObj = explainBuilder.done();
        return replyToQuery(explainObj);
    }

    // Do the work to generate the first batch of results. This blocks waiting to get responses from
    // the shard(s).
    std::vector<BSONObj> batch;

    // 0 means the cursor is exhausted. Otherwise we assume that a cursor with the returned id can
    // be retrieved via the ClusterCursorManager.
    CursorId cursorId;
    try {
        cursorId = ClusterFind::runQuery(
            opCtx, *canonicalQuery, ReadPreferenceSetting::get(opCtx), &batch);
    } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>&) {
        uasserted(40247, "OP_QUERY not supported on views");
    }

    // Fill out the response buffer.
    int numResults = 0;
    OpQueryReplyBuilder reply;
    for (auto&& obj : batch) {
        obj.appendSelfToBufBuilder(reply.bufBuilderForResults());
        numResults++;
    }

    return DbResponse{reply.toQueryReply(0,  // query result flags
                                         numResults,
                                         0,  // startingFrom
                                         cursorId)};
}

DbResponse Strategy::clientCommand(OperationContext* opCtx, const Message& m) {
    auto reply = rpc::makeReplyBuilder(rpc::protocolForMessage(m));
    BSONObjBuilder errorBuilder;

    bool propagateException = false;

    try {
        // Parse.
        OpMsgRequest request = [&] {
            try {
                return rpc::opMsgRequestFromAnyProtocol(m);
            } catch (const DBException& ex) {
                // If this error needs to fail the connection, propagate it out.
                if (ErrorCodes::isConnectionFatalMessageParseError(ex.code()))
                    propagateException = true;

                LOGV2_DEBUG(22769,
                            1,
                            "Exception thrown while parsing command {error}",
                            "Exception thrown while parsing command",
                            "error"_attr = redact(ex));
                throw;
            }
        }();

        opCtx->setExhaust(OpMsg::isFlagSet(m, OpMsg::kExhaustSupported));
        const auto session = opCtx->getClient()->session();
        if (session) {
            if (!opCtx->isExhaust() || request.getCommandName() != "isMaster"_sd) {
                InExhaustIsMaster::get(session.get())->setInExhaustIsMaster(false);
            }
        }

        // Execute.
        std::string db = request.getDatabase().toString();
        try {
            LOGV2_DEBUG(22770,
                        3,
                        "Command begin db: {db} msg id: {headerId}",
                        "Command begin",
                        "db"_attr = db,
                        "headerId"_attr = m.header().getId());
            runCommand(opCtx, request, m.operation(), reply.get(), &errorBuilder);
            LOGV2_DEBUG(22771,
                        3,
                        "Command end db: {db} msg id: {headerId}",
                        "Command end",
                        "db"_attr = db,
                        "headerId"_attr = m.header().getId());
        } catch (const DBException& ex) {
            LOGV2_DEBUG(
                22772,
                1,
                "Exception thrown while processing command on {db} msg id: {headerId} {error}",
                "Exception thrown while processing command",
                "db"_attr = db,
                "headerId"_attr = m.header().getId(),
                "error"_attr = redact(ex));

            // Record the exception in CurOp.
            CurOp::get(opCtx)->debug().errInfo = ex.toStatus();
            throw;
        }
    } catch (const DBException& ex) {
        if (propagateException) {
            throw;
        }
        reply->reset();
        auto bob = reply->getBodyBuilder();
        CommandHelpers::appendCommandStatusNoThrow(bob, ex.toStatus());
        appendRequiredFieldsToResponse(opCtx, &bob);
        bob.appendElements(errorBuilder.obj());
    }

    if (OpMsg::isFlagSet(m, OpMsg::kMoreToCome)) {
        return {};  // Don't reply.
    }

    DbResponse dbResponse;
    if (OpMsg::isFlagSet(m, OpMsg::kExhaustSupported)) {
        auto responseObj = reply->getBodyBuilder().asTempObj();
        if (responseObj.getField("ok").trueValue()) {
            dbResponse.shouldRunAgainForExhaust = reply->shouldRunAgainForExhaust();
            dbResponse.nextInvocation = reply->getNextInvocation();
        }
    }
    dbResponse.response = reply->done();

    return dbResponse;
}

DbResponse Strategy::getMore(OperationContext* opCtx, const NamespaceString& nss, DbMessage* dbm) {
    const int ntoreturn = dbm->pullInt();
    uassert(
        34424, str::stream() << "Invalid ntoreturn for OP_GET_MORE: " << ntoreturn, ntoreturn >= 0);
    const long long cursorId = dbm->pullInt64();

    globalOpCounters.gotGetMore();

    // TODO: Handle stale config exceptions here from coll being dropped or sharded during op for
    // now has same semantics as legacy request.

    auto statusGetDb = Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, nss.db());
    if (statusGetDb == ErrorCodes::NamespaceNotFound) {
        return replyToQuery(ResultFlag_CursorNotFound, nullptr, 0, 0);
    }
    uassertStatusOK(statusGetDb);

    boost::optional<std::int64_t> batchSize;
    if (ntoreturn) {
        batchSize = ntoreturn;
    }

    GetMoreRequest getMoreRequest(nss, cursorId, batchSize, boost::none, boost::none, boost::none);

    // Set the upconverted getMore as the CurOp command object.
    CurOp::get(opCtx)->setGenericOpRequestDetails(
        opCtx, nss, nullptr, getMoreRequest.toBSON(), dbm->msg().operation());

    auto cursorResponse = ClusterFind::runGetMore(opCtx, getMoreRequest);
    if (cursorResponse == ErrorCodes::CursorNotFound) {
        return replyToQuery(ResultFlag_CursorNotFound, nullptr, 0, 0);
    }
    uassertStatusOK(cursorResponse.getStatus());

    // Build the response document.
    BufBuilder buffer(FindCommon::kInitReplyBufferSize);

    int numResults = 0;
    for (const auto& obj : cursorResponse.getValue().getBatch()) {
        buffer.appendBuf((void*)obj.objdata(), obj.objsize());
        ++numResults;
    }

    return replyToQuery(0,
                        buffer.buf(),
                        buffer.len(),
                        numResults,
                        cursorResponse.getValue().getNumReturnedSoFar().value_or(0),
                        cursorResponse.getValue().getCursorId());
}

void Strategy::killCursors(OperationContext* opCtx, DbMessage* dbm) {
    const int numCursors = dbm->pullInt();
    massert(34425,
            str::stream() << "Invalid killCursors message. numCursors: " << numCursors
                          << ", message size: " << dbm->msg().dataSize() << ".",
            dbm->msg().dataSize() == 8 + (8 * numCursors));
    uassert(28794,
            str::stream() << "numCursors must be between 1 and 29999.  numCursors: " << numCursors
                          << ".",
            numCursors >= 1 && numCursors < 30000);

    globalOpCounters.gotOp(dbKillCursors, false);

    ConstDataCursor cursors(dbm->getArray(numCursors));

    Client* const client = opCtx->getClient();
    ClusterCursorManager* const manager = Grid::get(opCtx)->getCursorManager();

    for (int i = 0; i < numCursors; ++i) {
        const CursorId cursorId = cursors.readAndAdvance<LittleEndian<int64_t>>();

        boost::optional<NamespaceString> nss = manager->getNamespaceForCursorId(cursorId);
        if (!nss) {
            LOGV2_DEBUG(22773,
                        3,
                        "Can't find cursor to kill, no namespace found. Cursor id: {cursorId}",
                        "Can't find cursor to kill, no namespace found",
                        "cursorId"_attr = cursorId);
            continue;
        }

        auto authzSession = AuthorizationSession::get(client);
        auto authChecker = [&authzSession, &nss](UserNameIterator userNames) -> Status {
            return authzSession->checkAuthForKillCursors(*nss, userNames);
        };
        auto authzStatus = manager->checkAuthForKillCursors(opCtx, *nss, cursorId, authChecker);
        audit::logKillCursorsAuthzCheck(client, *nss, cursorId, authzStatus.code());
        if (!authzStatus.isOK()) {
            LOGV2_DEBUG(
                22774,
                3,
                "Not authorized to kill cursor. Namespace: '{namespace}', cursor id: {cursorId}",
                "Not authorized to kill cursor",
                "namespace"_attr = *nss,
                "cursorId"_attr = cursorId);
            continue;
        }

        Status killCursorStatus = manager->killCursor(opCtx, *nss, cursorId);
        if (!killCursorStatus.isOK()) {
            LOGV2_DEBUG(
                22775,
                3,
                "Can't find cursor to kill. Namespace: '{namespace}', cursor id: {cursorId}",
                "Can't find cursor to kill",
                "namespace"_attr = *nss,
                "cursorId"_attr = cursorId);
            continue;
        }

        LOGV2_DEBUG(22776,
                    3,
                    "Killed cursor. Namespace: '{namespace}', cursor id: {cursorId}",
                    "Killed cursor",
                    "namespace"_attr = *nss,
                    "cursorId"_attr = cursorId);
    }
}

void Strategy::writeOp(OperationContext* opCtx, DbMessage* dbm) {
    const auto& msg = dbm->msg();
    rpc::OpMsgReplyBuilder reply;
    BSONObjBuilder errorBuilder;
    runCommand(opCtx,
               [&]() {
                   switch (msg.operation()) {
                       case dbInsert: {
                           return InsertOp::parseLegacy(msg).serialize({});
                       }
                       case dbUpdate: {
                           return UpdateOp::parseLegacy(msg).serialize({});
                       }
                       case dbDelete: {
                           return DeleteOp::parseLegacy(msg).serialize({});
                       }
                       default:
                           MONGO_UNREACHABLE;
                   }
               }(),
               msg.operation(),
               &reply,
               &errorBuilder);  // built objects are ignored
}

void Strategy::explainFind(OperationContext* opCtx,
                           const BSONObj& findCommand,
                           const QueryRequest& qr,
                           ExplainOptions::Verbosity verbosity,
                           const ReadPreferenceSetting& readPref,
                           BSONObjBuilder* out) {
    const auto explainCmd = ClusterExplain::wrapAsExplain(findCommand, verbosity);

    long long millisElapsed;
    std::vector<AsyncRequestsSender::Response> shardResponses;

    for (int tries = 0;; ++tries) {
        bool canRetry = tries < 4;  // Fifth try (i.e. try #4) is the last one.

        // We will time how long it takes to run the commands on the shards.
        Timer timer;
        try {
            const auto routingInfo = uassertStatusOK(
                Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, qr.nss()));
            shardResponses =
                scatterGatherVersionedTargetByRoutingTable(opCtx,
                                                           qr.nss().db(),
                                                           qr.nss(),
                                                           routingInfo,
                                                           explainCmd,
                                                           readPref,
                                                           Shard::RetryPolicy::kIdempotent,
                                                           qr.getFilter(),
                                                           qr.getCollation());
            millisElapsed = timer.millis();
            break;
        } catch (ExceptionFor<ErrorCodes::ShardInvalidatedForTargeting>&) {
            Grid::get(opCtx)->catalogCache()->setOperationShouldBlockBehindCatalogCacheRefresh(
                opCtx, true);

            if (canRetry) {
                continue;
            }
            throw;
        } catch (const ExceptionForCat<ErrorCategory::NeedRetargettingError>& ex) {
            const auto staleNs = [&] {
                if (auto staleInfo = ex.extraInfo<StaleConfigInfo>()) {
                    return staleInfo->getNss();
                }
                throw;
            }();

            if (auto staleInfo = ex.extraInfo<StaleConfigInfo>()) {
                Grid::get(opCtx)
                    ->catalogCache()
                    ->invalidateShardOrEntireCollectionEntryForShardedCollection(
                        opCtx,
                        staleNs,
                        staleInfo->getVersionWanted(),
                        staleInfo->getVersionReceived(),
                        staleInfo->getShardId());
            } else {
                // If we don't have the stale config info and therefore don't know the shard's id,
                // we have to force all further targetting requests for the namespace to block on
                // a refresh.
                Grid::get(opCtx)->catalogCache()->onEpochChange(staleNs);
            }

            if (canRetry) {
                continue;
            }
            throw;
        } catch (const ExceptionFor<ErrorCodes::StaleDbVersion>& ex) {
            // Mark database entry in cache as stale.
            Grid::get(opCtx)->catalogCache()->onStaleDatabaseVersion(ex->getDb(),
                                                                     ex->getVersionReceived());
            if (canRetry) {
                continue;
            }
            throw;
        } catch (const ExceptionForCat<ErrorCategory::SnapshotError>&) {
            // Simple retry on any type of snapshot error.
            if (canRetry) {
                continue;
            }
            throw;
        }
    }

    const char* mongosStageName =
        ClusterExplain::getStageNameForReadOp(shardResponses.size(), findCommand);

    uassertStatusOK(ClusterExplain::buildExplainResult(
        opCtx, shardResponses, mongosStageName, millisElapsed, out));
}
}  // namespace mongo
