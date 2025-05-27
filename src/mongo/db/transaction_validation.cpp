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

#include "mongo/db/transaction_validation.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {


bool isRetryableWriteCommand(Service* service, StringData cmdName) {
    auto command = CommandHelpers::findCommand(service, cmdName);
    uassert(ErrorCodes::CommandNotFound,
            str::stream() << "Encountered unknown command during retryability check: " << cmdName,
            command);
    return command->supportsRetryableWrite();
}

bool isTransactionCommand(Service* service, StringData cmdName) {
    // TODO SERVER-82282 refactor: This code runs when commands are invoked from both mongod and
    // mongos and the latter does not know _shardsvrCreateCommand.
    if (cmdName == "_shardsvrCreateCollection")
        return false;

    auto command = CommandHelpers::findCommand(service, cmdName);
    uassert(ErrorCodes::CommandNotFound,
            str::stream() << "Encountered unknown command during isTransactionCommand check: "
                          << cmdName,
            command);
    return command->isTransactionCommand();
}

void validateWriteConcernForTransaction(const WriteConcernOptions& wcResult, const Command* cmd) {
    uassert(ErrorCodes::InvalidOptions,
            "writeConcern is not allowed within a multi-statement transaction",
            wcResult.usedDefaultConstructedWC || cmd->isTransactionCommand());
}

bool isReadConcernLevelAllowedInTransaction(repl::ReadConcernLevel readConcernLevel) {
    return readConcernLevel == repl::ReadConcernLevel::kSnapshotReadConcern ||
        readConcernLevel == repl::ReadConcernLevel::kMajorityReadConcern ||
        readConcernLevel == repl::ReadConcernLevel::kLocalReadConcern;
}

namespace {
const CommandNameAtom killCursorsAtom("killCursors");
const CommandNameAtom prepareTransactionAtom(PrepareTransaction::kCommandName);
const CommandNameAtom commitTransactionAtom(CommitTransaction::kCommandName);
const CommandNameAtom abortTransactionAtom(AbortTransaction::kCommandName);
}  // namespace

void validateSessionOptions(const OperationSessionInfoFromClient& sessionOptions,
                            Command* command,
                            const std::vector<NamespaceString>& namespaces,
                            bool allowTransactionsOnConfigDatabase) {

    if (sessionOptions.getAutocommit()) {
        CommandHelpers::canUseTransactions(namespaces, command, allowTransactionsOnConfigDatabase);
    }

    if (!sessionOptions.getAutocommit() && sessionOptions.getTxnNumber()) {
        uassert(ErrorCodes::NotARetryableWriteCommand,
                fmt::format(
                    "txnNumber may only be provided for multi-document transactions and retryable "
                    "write commands. autocommit:false was not provided, and {} is not a retryable "
                    "write command.",
                    command->getName()),
                command->supportsRetryableWrite());
    }

    if (sessionOptions.getStartTransaction()) {
        auto atom = command->getNameAtom();
        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                "Cannot run killCursors as the first operation in a multi-document transaction.",
                atom != killCursorsAtom);

        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                "Cannot start a transaction with a prepare",
                atom != prepareTransactionAtom);

        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                "Cannot start a transaction with a commit",
                atom != commitTransactionAtom);

        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                "Cannot start a transaction with an abort",
                atom != abortTransactionAtom);
    }
}

void doTransactionValidationForWrites(OperationContext* opCtx, const NamespaceString& ns) {
    if (!opCtx->inMultiDocumentTransaction())
        return;
    uassert(50791,
            str::stream() << "Cannot write to system collection " << ns.toStringForErrorMsg()
                          << " within a transaction.",
            !ns.isSystem() || ns.isPrivilegeCollection() || ns.isTimeseriesBucketsCollection());
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    uassert(50790,
            str::stream() << "Cannot write to unreplicated collection " << ns.toStringForErrorMsg()
                          << " within a transaction.",
            !replCoord->isOplogDisabledFor(opCtx, ns));
}
}  // namespace mongo
