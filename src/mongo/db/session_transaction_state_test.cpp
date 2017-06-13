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

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/base/init.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/session_transaction_table.h"
#include "mongo/db/session_txn_state_holder.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(SessionTxnStateHolder, Demo) {
    SessionTransactionTable table(nullptr);
    auto txnStateHolder = table.getSessionTxnState(LogicalSessionId::gen());

    OperationContextNoop opCtx;

    {
        // Caller has now control of txn state and can read/write from it.
        auto txnStateToken = txnStateHolder->getTransactionState(&opCtx, 1);

        auto partialResults = txnStateToken.get()->getPartialResults();

        // Go over request object, and mark all statements that has results in partialResults as
        // done.

        // For every statement that is not yet 'done':
        // Perform write op, and then store result:

        // Commented out since OperationContextNoop uses LockerNoop
        // SingleWriteResult result;
        // repl::OpTime opTime;
        // StmtId stmtId = 0;
        // txnStateToken.get()->storePartialResult(&opCtx, stmtId, result, opTime);

        // Consolidate partial results into final results for command response
    }

    // Caller now releases txnStateToken, other threads can now get a chance to access it.
}

}  // namespace mongo
