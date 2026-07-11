// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/util/modules.h"

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
