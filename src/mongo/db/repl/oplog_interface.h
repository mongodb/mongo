// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/record_id.h"
#include "mongo/db/transaction/transaction_history_iterator.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <memory>
#include <string>
#include <utility>

namespace mongo {
namespace repl {

class [[MONGO_MOD_OPEN]] OplogInterface {
    OplogInterface(const OplogInterface&) = delete;
    OplogInterface& operator=(const OplogInterface&) = delete;

public:
    class Iterator;

    virtual ~OplogInterface() = default;

    /**
     * Diagnostic information.
     */
    virtual std::string toString() const = 0;

    /**
     * Produces an iterator over oplog collection in reverse natural order.
     */
    virtual std::unique_ptr<Iterator> makeIterator() const = 0;

    /**
     * Produces an iterator that returns operations within a transaction.  Valid only for local
     * oplogs.
     */
    virtual std::unique_ptr<TransactionHistoryIteratorBase> makeTransactionHistoryIterator(
        const OpTime& startingOpTime, bool permitYield = false) const = 0;

    /**
     * The host and port of the server.
     */
    virtual HostAndPort hostAndPort() const = 0;

protected:
    OplogInterface() = default;
};

class OplogInterface::Iterator {
    Iterator(const Iterator&) = delete;
    Iterator& operator=(const Iterator&) = delete;

public:
    using Value = std::pair<BSONObj, RecordId>;

    Iterator() = default;
    virtual ~Iterator() = default;

    /**
     * Returns next operation and record id (if applicable) in the oplog.
     */
    virtual StatusWith<Value> next() = 0;
};

}  // namespace repl
}  // namespace mongo
