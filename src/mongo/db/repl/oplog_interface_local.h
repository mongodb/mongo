// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/oplog_interface.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/transaction/transaction_history_iterator.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <memory>
#include <string>

namespace mongo {

class OperationContext;

namespace repl {

/**
 * Scans local oplog collection in reverse natural order.
 */

class [[MONGO_MOD_PUBLIC]] OplogInterfaceLocal : public OplogInterface {
public:
    OplogInterfaceLocal(OperationContext* opCtx);
    std::string toString() const override;
    std::unique_ptr<OplogInterface::Iterator> makeIterator() const override;
    std::unique_ptr<TransactionHistoryIteratorBase> makeTransactionHistoryIterator(
        const OpTime& startingOpTime, bool permitYield = false) const override;
    HostAndPort hostAndPort() const override;

private:
    OperationContext* _opCtx;
};

}  // namespace repl
}  // namespace mongo
