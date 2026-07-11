// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/write_ops/unified_write_executor/write_op_producer.h"

namespace mongo {
namespace unified_write_executor {

boost::optional<WriteOp> WriteOpProducer::peekNext() {
    if (_stopProducingOps || _activeIndices.empty()) {
        return boost::none;
    }
    return WriteOp(_cmdRef.getOp(*_activeIndices.begin()));
}

void WriteOpProducer::advance() {
    if (!_activeIndices.empty()) {
        _activeIndices.erase(_activeIndices.begin());
    }
}

std::vector<WriteOp> WriteOpProducer::consumeAllRemainingOps() {
    if (_stopProducingOps || _activeIndices.empty()) {
        return {};
    }

    std::vector<WriteOp> result;
    result.reserve(_activeIndices.size());

    for (WriteOpId opId : _activeIndices) {
        result.emplace_back(_cmdRef.getOp(opId));
    }

    _activeIndices.clear();
    return result;
}

void WriteOpProducer::markOpReprocess(const WriteOp& op) {
    _activeIndices.insert(getWriteOpId(op));
}

}  // namespace unified_write_executor
}  // namespace mongo
