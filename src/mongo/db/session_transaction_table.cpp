/**
 *    Copyright (C) 2017 MongoDB, Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kWrite

#include "mongo/platform/basic.h"

#include "mongo/db/session_transaction_table.h"

#include <boost/optional.hpp>

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const auto sessionTransactionTableDecoration =
    ServiceContext::declareDecoration<boost::optional<SessionTransactionTable>>();

}  // namespace

SessionTransactionTable::SessionTransactionTable(ServiceContext* serviceContext)
    : _serviceContext(serviceContext) {}

void SessionTransactionTable::create(ServiceContext* service) {
    auto& sessionTransactionTable = sessionTransactionTableDecoration(service);
    invariant(!sessionTransactionTable);

    sessionTransactionTable.emplace(service);
}

SessionTransactionTable* SessionTransactionTable::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

SessionTransactionTable* SessionTransactionTable::get(ServiceContext* service) {
    auto& sessionTransactionTable = sessionTransactionTableDecoration(service);
    invariant(sessionTransactionTable);

    return sessionTransactionTable.get_ptr();
}

void SessionTransactionTable::onStepUp(OperationContext* opCtx) {
    DBDirectClient client(opCtx);

    const size_t initialExtentSize = 0;
    const bool capped = false;
    const bool maxSize = 0;

    BSONObj result;

    if (client.createCollection(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                                initialExtentSize,
                                capped,
                                maxSize,
                                &result)) {
        return;
    }

    const auto status = getStatusFromCommandResult(result);

    if (status == ErrorCodes::NamespaceExists) {
        return;
    }

    uasserted(status.code(),
              str::stream() << "Failed to create the "
                            << NamespaceString::kSessionTransactionsTableNamespace.ns()
                            << " collection due to "
                            << status.reason());
}

std::shared_ptr<SessionTxnStateHolder> SessionTransactionTable::getSessionTxnState(
    const LogicalSessionId& sessionId) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    auto sessionTxnStateIter = _txnTable.find(sessionId);

    if (sessionTxnStateIter == _txnTable.end()) {
        // TODO: consult sessions table (without network I/O). The fact that we reached this point
        // means that the session was previously active.

        auto txnState = stdx::make_unique<SessionTxnState>(sessionId, kUninitializedTxnNumber);
        auto newEntry = std::make_shared<SessionTxnStateHolder>(std::move(txnState));
        _txnTable.insert(std::make_pair(sessionId, newEntry));
        return newEntry;
    }

    auto& sessionTxnState = sessionTxnStateIter->second;
    invariant(sessionTxnState);
    return sessionTxnState;
}

void SessionTransactionTable::cleanupInactiveSessions(OperationContext* opCtx) {
    // TODO: Get from active session list from session cache
}

}  // namespace mongo
