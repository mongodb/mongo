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

#include "mongo/db/repl/oplog_interface_remote.h"

#include "mongo/client/dbclient_base.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/util/str.h"

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
                                           StringData collectionName,
                                           int batchSize)
    : _hostAndPort(hostAndPort),
      _getConnection(getConnection),
      _collectionName(collectionName),
      _batchSize(batchSize) {}

std::string OplogInterfaceRemote::toString() const {
    return _getConnection()->toString();
}

std::unique_ptr<OplogInterface::Iterator> OplogInterfaceRemote::makeIterator() const {
    FindCommandRequest findRequest{NamespaceString{_collectionName}};
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
