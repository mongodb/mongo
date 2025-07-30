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

#pragma once

#include "mongo/db/commands/query_cmd/bulk_write_common.h"
#include "mongo/s/write_ops/write_command_ref.h"

#include <variant>

#include <boost/optional.hpp>

namespace mongo {
namespace unified_write_executor {
enum WriteType {
    kInsert = BatchedCommandRequest::BatchType_Insert,
    kUpdate = BatchedCommandRequest::BatchType_Update,
    kDelete = BatchedCommandRequest::BatchType_Delete,
    kFindAndMod,  // TODO SERVER-103949 will use this type or remove it.
};

using WriteOpId = size_t;

using WriteOpContext = WriteCommandRef;

class WriteOp {
public:
    WriteOp(WriteOpRef ref) : _ref(std::move(ref)) {}

    WriteOp(const BatchedCommandRequest& request, int index)
        : WriteOp(WriteOpRef{request, index}) {}

    WriteOp(const BulkWriteCommandRequest& request, int index)
        : WriteOp(WriteOpRef{request, index}) {}

    WriteOpId getId() const {
        return _ref.getIndex();
    }

    const NamespaceString& getNss() const {
        return _ref.getNss();
    }

    WriteType getType() const {
        return WriteType(_ref.getOpType());
    }

    BulkWriteOpVariant getBulkWriteOp() const {
        return _ref.visitOpData(
            OverloadedVisitor{[&](const BSONObj& insertDoc) -> BulkWriteOpVariant {
                                  return BulkWriteInsertOp(0, insertDoc);
                              },
                              [&](const write_ops::UpdateOpEntry& updateOp) -> BulkWriteOpVariant {
                                  return bulk_write_common::toBulkWriteUpdate(updateOp);
                              },
                              [&](const write_ops::DeleteOpEntry& deleteOp) -> BulkWriteOpVariant {
                                  return bulk_write_common::toBulkWriteDelete(deleteOp);
                              },
                              [&](const mongo::BulkWriteInsertOp& insertOp) -> BulkWriteOpVariant {
                                  return insertOp;
                              },
                              [&](const mongo::BulkWriteUpdateOp& updateOp) -> BulkWriteOpVariant {
                                  return updateOp;
                              },
                              [&](const mongo::BulkWriteDeleteOp& deleteOp) -> BulkWriteOpVariant {
                                  return deleteOp;
                              }});
    }

    bool isMulti() const {
        return _ref.getMulti();
    }

    WriteCommandRef getCommand() const {
        return _ref.getCommand();
    }

    WriteOpRef getItemRef() const {
        return _ref;
    }

    int getEffectiveStmtId() const {
        auto cmdRef = _ref.getCommand();
        int index = _ref.getIndex();

        if (auto stmtIds = cmdRef.getStmtIds()) {
            return stmtIds->at(index);
        }

        auto stmtId = cmdRef.getStmtId();
        int32_t firstStmtId = stmtId ? *stmtId : 0;
        return firstStmtId + index;
    }

    friend bool operator==(const WriteOp& lhs, const WriteOp& rhs) = default;

    friend std::strong_ordering operator<=>(const WriteOp& lhs, const WriteOp& rhs) = default;

    template <typename H>
    friend H AbslHashValue(H h, const WriteOp& op) {
        return H::combine(std::move(h), op._ref);
    }

private:
    WriteOpRef _ref;
};

}  // namespace unified_write_executor
}  // namespace mongo
