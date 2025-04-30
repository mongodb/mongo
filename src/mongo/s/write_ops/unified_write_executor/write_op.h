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

#include <boost/optional.hpp>
#include <variant>

#include "mongo/s/write_ops/batched_command_request.h"


namespace mongo {
namespace unified_write_executor {
enum WriteType {
    kInsert = BatchedCommandRequest::BatchType_Insert,
    kUpdate = BatchedCommandRequest::BatchType_Update,
    kDelete = BatchedCommandRequest::BatchType_Delete,
    kFindAndMod,  // TODO SERVER-103949 will use this type or remove it.
};

using WriteOpId = size_t;

class WriteOp {
public:
    WriteOp(const BulkWriteCommandRequest& request, int index) : _op(&request, index) {}

    WriteOpId getId() const {
        return _op.getItemIndex();
    }

    const NamespaceString& getNss() const {
        return _op.getNss();
    }

    WriteType getType() const {
        return WriteType(_op.getOpType());
    }

    BatchItemRef getRef() const {
        return _op;
    }

    bool isMulti() const {
        return _op.isMulti();
    }

private:
    BatchItemRef _op;
};

}  // namespace unified_write_executor
}  // namespace mongo
