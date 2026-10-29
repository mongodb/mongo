// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Returns true if the given command name can run as a retryable write.
 */
bool isRetryableWriteCommand(Service* service, std::string_view cmdName);

/**
 * Returns true if the given cmd name is a transaction control command.  These are also the only
 * commands allowed to specify write concern in a transaction.
 */
bool isTransactionCommand(Service* service, std::string_view cmdName);

/**
 * Throws if the given write concern is not allowed in a transaction.
 */
void validateWriteConcernForTransaction(const WriteConcernOptions& wcResult,
                                        const Command* command);

/**
 * Returns true if the given readConcern level is valid for use in a transaction.
 */
bool isReadConcernLevelAllowedInTransaction(repl::ReadConcernLevel readConcernLevel);

/**
 * Throws if the given session options are invalid for the given command and target namespace.
 */
void validateSessionOptions(const OperationSessionInfoFromClient& sessionOptions,
                            Command* command,
                            const std::vector<NamespaceString>& namespaces,
                            bool allowTransactionsOnConfigDatabase);

/**
 * Throws if the specified namespace refers to a systems collection that is not
 * allowed to be modified via a transaction, or if the specified namespace
 * refers to a collection that is unreplicated.
 */
void doTransactionValidationForWrites(OperationContext* opCtx, const NamespaceString& ns);

}  // namespace mongo
