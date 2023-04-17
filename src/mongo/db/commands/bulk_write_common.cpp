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

#include "mongo/db/commands/bulk_write_common.h"

#include "mongo/db/commands/bulk_write_crud_op.h"

namespace mongo {
namespace bulk_write_common {

void validateRequest(const BulkWriteCommandRequest& req, bool isRetryableWrite) {
    const auto& ops = req.getOps();
    const auto& nsInfo = req.getNsInfo();

    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "May not specify both stmtId and stmtIds in bulkWrite command. Got "
                          << BSON("stmtId" << *req.getStmtId() << "stmtIds" << *req.getStmtIds())
                          << ". BulkWrite command: " << req.toBSON({}),
            !(req.getStmtId() && req.getStmtIds()));

    if (const auto& stmtIds = req.getStmtIds()) {
        uassert(
            ErrorCodes::InvalidLength,
            str::stream() << "Number of statement ids must match the number of batch entries. Got "
                          << stmtIds->size() << " statement ids but " << ops.size()
                          << " operations. Statement ids: " << BSON("stmtIds" << *stmtIds)
                          << ". BulkWrite command: " << req.toBSON({}),
            stmtIds->size() == ops.size());
    }

    // Validate that every ops entry has a valid nsInfo index.
    // Also validate that we only have one findAndModify for retryable writes.
    bool seenFindAndModify = false;
    for (const auto& op : ops) {
        const auto& bulkWriteOp = BulkWriteCRUDOp(op);
        unsigned int nsInfoIdx = bulkWriteOp.getNsInfoIdx();
        uassert(ErrorCodes::BadValue,
                str::stream() << "BulkWrite ops entry " << bulkWriteOp.toBSON()
                              << " has an invalid nsInfo index.",
                nsInfoIdx < nsInfo.size());

        if (isRetryableWrite) {
            switch (bulkWriteOp.getType()) {
                case BulkWriteCRUDOp::kInsert:
                    break;
                case BulkWriteCRUDOp::kUpdate: {
                    auto update = bulkWriteOp.getUpdate();
                    if (update->getReturn()) {
                        uassert(
                            ErrorCodes::BadValue,
                            "BulkWrite can only support 1 op with a return for a retryable write",
                            !seenFindAndModify);
                        seenFindAndModify = true;
                    }
                    break;
                }
                case BulkWriteCRUDOp::kDelete: {
                    auto deleteOp = bulkWriteOp.getDelete();
                    if (deleteOp->getReturn()) {
                        uassert(
                            ErrorCodes::BadValue,
                            "BulkWrite can only support 1 op with a return for a retryable write",
                            !seenFindAndModify);
                        seenFindAndModify = true;
                    }
                    break;
                }
            }
        }
    }
}

std::vector<Privilege> getPrivileges(const BulkWriteCommandRequest& req) {
    const auto& ops = req.getOps();
    const auto& nsInfo = req.getNsInfo();

    std::vector<Privilege> privileges;
    privileges.reserve(nsInfo.size());
    ActionSet actions;
    if (req.getBypassDocumentValidation()) {
        actions.addAction(ActionType::bypassDocumentValidation);
    }

    // Create initial Privilege entry for each nsInfo entry.
    for (const auto& ns : nsInfo) {
        privileges.emplace_back(ResourcePattern::forExactNamespace(ns.getNs()), actions);
    }

    // Iterate over each op and assign the appropriate actions to the namespace privilege.
    for (const auto& op : ops) {
        const auto& bulkWriteOp = BulkWriteCRUDOp(op);
        ActionSet newActions = bulkWriteOp.getActions();
        unsigned int nsInfoIdx = bulkWriteOp.getNsInfoIdx();
        uassert(ErrorCodes::BadValue,
                str::stream() << "BulkWrite ops entry " << bulkWriteOp.toBSON()
                              << " has an invalid nsInfo index.",
                nsInfoIdx < nsInfo.size());

        auto& privilege = privileges[nsInfoIdx];
        privilege.addActions(newActions);
    }

    return privileges;
}

}  // namespace bulk_write_common
}  // namespace mongo
