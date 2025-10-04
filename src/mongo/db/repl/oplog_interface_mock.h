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

namespace MONGO_MOD_PARENT_PRIVATE mongo {
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
}  // namespace MONGO_MOD_PARENT_PRIVATE mongo
