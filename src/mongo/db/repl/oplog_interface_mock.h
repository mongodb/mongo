// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog_interface.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/transaction/transaction_history_iterator.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <initializer_list>
#include <list>
#include <memory>
#include <string>
#include <utility>

namespace [[MONGO_MOD_PARENT_PRIVATE]] mongo {
class TransactionHistoryIteratorBase;

namespace repl {

/**
 * Simulates oplog for testing rollback functionality.
 */
class OplogInterfaceMock : public OplogInterface {
    OplogInterfaceMock(const OplogInterfaceMock&) = delete;
    OplogInterfaceMock& operator=(const OplogInterfaceMock&) = delete;

public:
    using Operation = std::pair<BSONObj, RecordId>;
    using Operations = std::list<Operation>;
    OplogInterfaceMock() = default;
    explicit OplogInterfaceMock(std::initializer_list<Operation> operations);
    explicit OplogInterfaceMock(const Operations& operations);
    void setOperations(const Operations& operations);
    std::string toString() const override;
    std::unique_ptr<OplogInterface::Iterator> makeIterator() const override;
    std::unique_ptr<TransactionHistoryIteratorBase> makeTransactionHistoryIterator(
        const OpTime& startOpTime, bool permitYield = false) const override;
    HostAndPort hostAndPort() const override;

private:
    Operations _operations;
};

}  // namespace repl
}  // namespace mongo
