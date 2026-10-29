// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/s/write_ops/write_command_ref.h"
#include "mongo/s/write_ops/write_op_helper.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo {
namespace unified_write_executor {
enum WriteType {
    kInsert = BatchedCommandRequest::BatchType_Insert,
    kUpdate = BatchedCommandRequest::BatchType_Update,
    kDelete = BatchedCommandRequest::BatchType_Delete,
};

using WriteOpId = size_t;
using WriteOp = WriteOpRef;

inline WriteOpId getWriteOpId(const WriteOp& op) {
    return op.getIndex();
}

inline WriteType getWriteOpType(const WriteOp& op) {
    return WriteType(op.getOpType());
}
}  // namespace unified_write_executor
}  // namespace mongo
