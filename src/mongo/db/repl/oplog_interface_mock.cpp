// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/oplog_interface_mock.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/transaction/transaction_history_iterator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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
                          << _nextOpTime.toBSON() << " cannot be found");
        }
        // We shouldn't get any other error.
        MONGO_UNREACHABLE;
    }

    repl::OpTime nextOpTime(OperationContext*) override {
        MONGO_UNREACHABLE;
    }

    ~TransactionHistoryIteratorMock() override {}

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
