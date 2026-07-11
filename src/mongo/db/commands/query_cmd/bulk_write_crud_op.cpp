// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/query_cmd/bulk_write_crud_op.h"

#include "mongo/db/auth/action_type.h"

namespace mongo {

BulkWriteCRUDOp::BulkWriteCRUDOp(const BulkWriteOpVariant& op) : _op{op}, _type{op.index()} {}

BulkWriteCRUDOp::OpType BulkWriteCRUDOp::getType() const {
    return _type;
}

unsigned int BulkWriteCRUDOp::getNsInfoIdx() const {
    return visit(
        OverloadedVisitor{
            [](const auto& value) { return value.getNsInfoIdx(); },
        },
        _op);
}

ActionSet BulkWriteCRUDOp::getActions() const {
    ActionSet newActions;
    switch (_type) {
        case kInsert:
            newActions.addAction(ActionType::insert);
            break;
        case kUpdate: {
            if (getUpdate()->getUpsert()) {
                newActions.addAction(ActionType::insert);
            }
            newActions.addAction(ActionType::update);
            break;
        }
        case kDelete:
            newActions.addAction(ActionType::remove);
            break;
        default:
            MONGO_UNREACHABLE;
            break;
    }

    return newActions;
}

const mongo::BulkWriteInsertOp* BulkWriteCRUDOp::getInsert() const {
    return get_if<BulkWriteCRUDOp::OpType::kInsert>(&_op);
}

const mongo::BulkWriteUpdateOp* BulkWriteCRUDOp::getUpdate() const {
    return get_if<BulkWriteCRUDOp::OpType::kUpdate>(&_op);
}

const mongo::BulkWriteDeleteOp* BulkWriteCRUDOp::getDelete() const {
    return get_if<BulkWriteCRUDOp::OpType::kDelete>(&_op);
}

mongo::BSONObj BulkWriteCRUDOp::toBSON() const {
    return visit(
        OverloadedVisitor{
            [](const auto& value) { return value.toBSON(); },
        },
        _op);
}

}  // namespace mongo
