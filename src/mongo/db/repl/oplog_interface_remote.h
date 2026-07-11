// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/oplog_interface.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/transaction/transaction_history_iterator.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <functional>
#include <memory>
#include <string>

namespace mongo {

class DBClientBase;

namespace repl {

/**
 * Reads oplog on remote server.
 */

class [[MONGO_MOD_PARENT_PRIVATE]] OplogInterfaceRemote : public OplogInterface {
public:
    /**
     * Type of function to return a connection to the sync source.
     */
    using GetConnectionFn = std::function<DBClientBase*()>;

    OplogInterfaceRemote(HostAndPort hostAndPort, GetConnectionFn getConnection, int batchSize);
    std::string toString() const override;
    std::unique_ptr<OplogInterface::Iterator> makeIterator() const override;
    std::unique_ptr<TransactionHistoryIteratorBase> makeTransactionHistoryIterator(
        const OpTime& startingOpTime, bool permitYield = false) const override;
    HostAndPort hostAndPort() const override;

private:
    HostAndPort _hostAndPort;
    GetConnectionFn _getConnection;
    int _batchSize;
};

}  // namespace repl
}  // namespace mongo
