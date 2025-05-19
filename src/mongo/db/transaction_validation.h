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

#include "mongo/base/string_data.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/write_concern_options.h"

namespace mongo {

/**
 * Returns true if the given command name can run as a retryable write.
 */
bool isRetryableWriteCommand(Service* service, StringData cmdName);

/**
 * Returns true if the given cmd name is a transaction control command.  These are also the only
 * commands allowed to specify write concern in a transaction.
 */
bool isTransactionCommand(Service* service, StringData cmdName);

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
