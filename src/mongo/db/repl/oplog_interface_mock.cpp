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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/oplog_interface_mock.h"
#include "mongo/db/transaction_history_iterator.h"

namespace mongo {
namespace repl {

namespace {

class OplogIteratorMock : public OplogInterface::Iterator {
public:
    OplogIteratorMock(OplogInterfaceMock::Operations::const_iterator iterator,
                      OplogInterfaceMock::Operations::const_iterator iteratorEnd);
    StatusWith<Value> next() override;

private:
    OplogInterfaceMock::Operations::const_iterator _iterator;
    OplogInterfaceMock::Operations::const_iterator _iteratorEnd;
};

OplogIteratorMock::OplogIteratorMock(OplogInterfaceMock::Operations::const_iterator iter,
                                     OplogInterfaceMock::Operations::const_iterator iterEnd)
    : _iterator(iter), _iteratorEnd(iterEnd) {}

StatusWith<OplogInterface::Iterator::Value> OplogIteratorMock::next() {
    if (_iterator == _iteratorEnd) {
        return StatusWith<OplogInterface::Iterator::Value>(ErrorCodes::NoSuchKey, "no more ops");
    }
    return *(_iterator++);
}

class TransactionHistoryIteratorMock : public TransactionHistoryIteratorBase {
public:
    TransactionHistoryIteratorMock(const OpTime& startOpTime,
                                   std::unique_ptr<OplogInterface::Iterator> iter)
        : _nextOpTime(startOpTime), _iter(std::move(iter)) {}

    repl::OplogEntry next(OperationContext*) override {
        invariant(hasNext());
        auto operation = _iter->next();
        while (operation.isOK()) {
            auto& oplogBSON = operation.getValue().first;
            auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(oplogBSON));
            if (oplogEntry.getOpTime() == _nextOpTime) {
                const auto& oplogPrevTsOption = oplogEntry.getPrevWriteOpTimeInTransaction();
                uassert(
                    ErrorCodes::FailedToParse,
                    str::stream()
                        << "Missing prevTs field on oplog entry of previous write in transaction: "
                        << oplogBSON,
                    oplogPrevTsOption);

                _nextOpTime = oplogPrevTsOption.value();
                return oplogEntry;
            }
            operation = _iter->next();
        }
        if (operation.getStatus().code() == ErrorCodes::NoSuchKey) {
            uasserted(ErrorCodes::IncompleteTransactionHistory,
                      str::stream()
                          << "oplog no longer contains the complete write history of this "
                             "transaction, log with opTime "
                          << _nextOpTime.toBSON()
                          << " cannot be found");
        }
        // We shouldn't get any other error.
        MONGO_UNREACHABLE;
    }

    virtual ~TransactionHistoryIteratorMock() {}

    bool hasNext() const override {
        return !_nextOpTime.isNull();
    }

private:
    repl::OpTime _nextOpTime;
    std::unique_ptr<OplogInterface::Iterator> _iter;
};

}  // namespace

OplogInterfaceMock::OplogInterfaceMock(std::initializer_list<Operation> operations)
    : _operations(operations) {}

OplogInterfaceMock::OplogInterfaceMock(const Operations& operations) : _operations(operations) {}

void OplogInterfaceMock::setOperations(const Operations& operations) {
    _operations = operations;
}

std::string OplogInterfaceMock::toString() const {
    return "OplogInterfaceMock";
}

std::unique_ptr<OplogInterface::Iterator> OplogInterfaceMock::makeIterator() const {
    return std::unique_ptr<OplogInterface::Iterator>(
        new OplogIteratorMock(_operations.begin(), _operations.end()));
}

std::unique_ptr<TransactionHistoryIteratorBase> OplogInterfaceMock::makeTransactionHistoryIterator(
    const OpTime& startOpTime, bool permitYield) const {
    return std::make_unique<TransactionHistoryIteratorMock>(startOpTime, makeIterator());
}

HostAndPort OplogInterfaceMock::hostAndPort() const {
    // Returns a default-constructed HostAndPort, which has an empty hostname and an invalid port.
    return {};
}

}  // namespace repl
}  // namespace mongo
