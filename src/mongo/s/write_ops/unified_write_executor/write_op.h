/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include <boost/optional.hpp>
#include <variant>

#include "mongo/s/write_ops/batched_command_request.h"

namespace mongo {
namespace unified_write_executor {

class BulkWriteOp {
public:
    using BulkWriteOpVariant =
        std::variant<mongo::BulkWriteInsertOp, mongo::BulkWriteUpdateOp, mongo::BulkWriteDeleteOp>;

    BulkWriteOp(const BulkWriteCommandRequest& request, int index)
        : _request(request), _index(index) {}

    NamespaceInfoEntry getNsInfo() {
        auto nsIndex =
            visit(OverloadedVisitor{
                      [](const mongo::BulkWriteInsertOp& value) { return value.getInsert(); },
                      [](const mongo::BulkWriteUpdateOp& value) { return value.getUpdate(); },
                      [](const mongo::BulkWriteDeleteOp& value) {
                          return value.getDeleteCommand();
                      }},
                  getOp());
        return _request.getNsInfo()[nsIndex];
    }

    const BulkWriteOpVariant& getOp() const {
        return _request.getOps()[_index];
    }

    size_t getIndex() const {
        return _index;
    }

private:
    const BulkWriteCommandRequest& _request;
    size_t _index;
};

class WriteOp {
public:
    WriteOp(const BulkWriteCommandRequest& request, size_t index)
        : _op(BulkWriteOp(request, index)) {}

    BulkWriteOp getBulkWriteOp() const {
        return std::get<BulkWriteOp>(_op);
    }

    size_t getId() const {
        return visit(
            OverloadedVisitor{
                [](const BulkWriteOp& op) { return op.getIndex(); },
            },
            _op);
    }

private:
    std::variant<BulkWriteOp> _op;
};

}  // namespace unified_write_executor
}  // namespace mongo
