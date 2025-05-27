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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/commands/query_cmd/bulk_write_gen.h"

#include <cstddef>
#include <variant>

namespace mongo {

using BulkWriteOpVariant =
    std::variant<mongo::BulkWriteInsertOp, mongo::BulkWriteUpdateOp, mongo::BulkWriteDeleteOp>;

/**
 * The BulkWriteCRUDOp class makes working with
 * variant<BulkWriteInsertOp, BulkWriteUpdateOp, BulkWriteDeleteOp> easier.
 */
class BulkWriteCRUDOp {
public:
    enum OpType : size_t { kInsert = 0, kUpdate = 1, kDelete = 2 };

    BulkWriteCRUDOp(const BulkWriteOpVariant& op);

    OpType getType() const;
    unsigned int getNsInfoIdx() const;
    ActionSet getActions() const;
    mongo::BSONObj toBSON() const;

    const mongo::BulkWriteInsertOp* getInsert() const;
    const mongo::BulkWriteUpdateOp* getUpdate() const;
    const mongo::BulkWriteDeleteOp* getDelete() const;

private:
    const std::
        variant<mongo::BulkWriteInsertOp, mongo::BulkWriteUpdateOp, mongo::BulkWriteDeleteOp>& _op;
    OpType _type;
};

}  // namespace mongo
