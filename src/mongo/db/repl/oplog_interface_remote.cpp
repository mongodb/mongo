// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/oplog_interface_remote.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <utility>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>

namespace mongo {
namespace repl {

namespace {

class OplogIteratorRemote : public OplogInterface::Iterator {
public:
    OplogIteratorRemote(std::unique_ptr<DBClientCursor> cursor);
    StatusWith<Value> next() override;

private:
    std::unique_ptr<DBClientCursor> _cursor;
};

OplogIteratorRemote::OplogIteratorRemote(std::unique_ptr<DBClientCursor> cursor)
    : _cursor(std::move(cursor)) {}

StatusWith<OplogInterface::Iterator::Value> OplogIteratorRemote::next() {
    if (!_cursor.get()) {
        return StatusWith<Value>(ErrorCodes::NamespaceNotFound, "no cursor for remote oplog");
    }
    if (!_cursor->more()) {
        return StatusWith<Value>(ErrorCodes::CollectionIsEmpty,
                                 "no more operations in remote oplog");
    }
    return StatusWith<Value>(std::make_pair(_cursor->nextSafe(), RecordId()));
}

}  // namespace

OplogInterfaceRemote::OplogInterfaceRemote(HostAndPort hostAndPort,
                                           GetConnectionFn getConnection,
                                           int batchSize)
    : _hostAndPort(hostAndPort), _getConnection(getConnection), _batchSize(batchSize) {}

std::string OplogInterfaceRemote::toString() const {
    return _getConnection()->toString();
}

std::unique_ptr<OplogInterface::Iterator> OplogInterfaceRemote::makeIterator() const {
    FindCommandRequest findRequest{NamespaceString::kRsOplogNamespace};
    findRequest.setProjection(BSON("ts" << 1 << "t" << 1LL));
    findRequest.setSort(BSON("$natural" << -1));
    findRequest.setBatchSize(_batchSize);
    findRequest.setReadConcern(ReadConcernArgs::kLocal);
    return std::unique_ptr<OplogInterface::Iterator>(
        new OplogIteratorRemote(_getConnection()->find(std::move(findRequest))));
}

std::unique_ptr<TransactionHistoryIteratorBase>
OplogInterfaceRemote::makeTransactionHistoryIterator(const OpTime&, bool permitYield) const {
    // Should never ask for remote transaction history.
    MONGO_UNREACHABLE;
}

HostAndPort OplogInterfaceRemote::hostAndPort() const {
    return _hostAndPort;
}

}  // namespace repl
}  // namespace mongo
