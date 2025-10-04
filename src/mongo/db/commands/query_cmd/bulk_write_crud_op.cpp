/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
