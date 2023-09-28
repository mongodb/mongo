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

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/commands/bulk_write_crud_op.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {
namespace bulk_write_common {

void validateRequest(const BulkWriteCommandRequest& req, bool isRouter) {
    const auto& ops = req.getOps();
    const auto& nsInfos = req.getNsInfo();

    uassert(ErrorCodes::InvalidLength,
            str::stream() << "Write batch sizes must be between 1 and "
                          << write_ops::kMaxWriteBatchSize << ". Got " << ops.size()
                          << " operations.",
            ops.size() != 0 && ops.size() <= write_ops::kMaxWriteBatchSize);

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

    // Validate the namespaces in nsInfo.
    for (const auto& nsInfo : nsInfos) {
        auto& ns = nsInfo.getNs();
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid namespace specified for bulkWrite: '"
                              << nsInfo.getNs().toStringForErrorMsg() << "'",
                ns.isValid());
        uassert(7934201,
                "'isTimeseriesNamespace' parameter can only be set when the request is sent on "
                "'system.buckets' namespace to each shard",
                !nsInfo.getIsTimeseriesNamespace() ||
                    (!isRouter && ns.isTimeseriesBucketsCollection()));
    }

    // Validate that every ops entry has a valid nsInfo index.
    for (const auto& op : ops) {
        const auto& bulkWriteOp = BulkWriteCRUDOp(op);
        unsigned int nsInfoIdx = bulkWriteOp.getNsInfoIdx();
        uassert(ErrorCodes::BadValue,
                str::stream() << "BulkWrite ops entry " << bulkWriteOp.toBSON()
                              << " has an invalid nsInfo index.",
                nsInfoIdx < nsInfos.size());
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

int32_t getStatementId(const BulkWriteCommandRequest& req, size_t currentOpIdx) {
    auto stmtId = req.getStmtId();
    auto stmtIds = req.getStmtIds();

    if (stmtIds) {
        return stmtIds->at(currentOpIdx);
    }

    int32_t firstStmtId = stmtId ? *stmtId : 0;
    return firstStmtId + currentOpIdx;
}

NamespaceInfoEntry getFLENamespaceInfoEntry(const BSONObj& bulkWrite) {
    BulkWriteCommandRequest bulk =
        BulkWriteCommandRequest::parse(IDLParserContext("bulkWrite"), bulkWrite);
    const std::vector<NamespaceInfoEntry>& nss = bulk.getNsInfo();
    uassert(ErrorCodes::BadValue,
            "BulkWrite with Queryable Encryption supports only a single namespace",
            nss.size() == 1);
    return nss[0];
}

write_ops::InsertCommandRequest makeInsertCommandRequestForFLE(
    const std::vector<mongo::BSONObj>& documents,
    const BulkWriteCommandRequest& req,
    const mongo::NamespaceInfoEntry& nsInfoEntry) {
    write_ops::InsertCommandRequest request(nsInfoEntry.getNs(), documents);
    request.setDollarTenant(req.getDollarTenant());
    request.setExpectPrefix(req.getExpectPrefix());
    auto& requestBase = request.getWriteCommandRequestBase();
    requestBase.setEncryptionInformation(nsInfoEntry.getEncryptionInformation());
    requestBase.setOrdered(req.getOrdered());
    requestBase.setBypassDocumentValidation(req.getBypassDocumentValidation());
    requestBase.setCollectionUUID(nsInfoEntry.getCollectionUUID());

    return request;
}

write_ops::UpdateOpEntry makeUpdateOpEntryFromUpdateOp(const BulkWriteUpdateOp* op) {
    write_ops::UpdateOpEntry update;
    update.setQ(op->getFilter());
    update.setMulti(op->getMulti());
    update.setC(op->getConstants());
    update.setU(op->getUpdateMods());
    update.setHint(op->getHint());
    update.setCollation(op->getCollation());
    update.setArrayFilters(op->getArrayFilters().value_or(std::vector<BSONObj>()));
    update.setUpsert(op->getUpsert());
    update.setUpsertSupplied(op->getUpsertSupplied());
    update.setAllowShardKeyUpdatesWithoutFullShardKeyInQuery(
        op->getAllowShardKeyUpdatesWithoutFullShardKeyInQuery());
    return update;
}

write_ops::UpdateCommandRequest makeUpdateCommandRequestFromUpdateOp(
    const BulkWriteUpdateOp* op, const BulkWriteCommandRequest& req, size_t currentOpIdx) {
    auto idx = op->getUpdate();
    auto nsEntry = req.getNsInfo()[idx];

    auto stmtId = bulk_write_common::getStatementId(req, currentOpIdx);

    std::vector<write_ops::UpdateOpEntry> updates{makeUpdateOpEntryFromUpdateOp(op)};
    write_ops::UpdateCommandRequest updateCommand(nsEntry.getNs(), updates);

    updateCommand.setDollarTenant(req.getDollarTenant());
    updateCommand.setExpectPrefix(req.getExpectPrefix());
    updateCommand.setLet(req.getLet());

    updateCommand.getWriteCommandRequestBase().setIsTimeseriesNamespace(
        nsEntry.getIsTimeseriesNamespace());
    updateCommand.getWriteCommandRequestBase().setCollectionUUID(nsEntry.getCollectionUUID());

    updateCommand.getWriteCommandRequestBase().setEncryptionInformation(
        nsEntry.getEncryptionInformation());
    updateCommand.getWriteCommandRequestBase().setBypassDocumentValidation(
        req.getBypassDocumentValidation());

    updateCommand.getWriteCommandRequestBase().setStmtIds(std::vector<StmtId>{stmtId});
    updateCommand.getWriteCommandRequestBase().setOrdered(req.getOrdered());

    return updateCommand;
}

write_ops::DeleteCommandRequest makeDeleteCommandRequestForFLE(
    OperationContext* opCtx,
    const BulkWriteDeleteOp* op,
    const BulkWriteCommandRequest& req,
    const mongo::NamespaceInfoEntry& nsInfoEntry) {
    write_ops::DeleteOpEntry deleteEntry;
    if (op->getCollation()) {
        deleteEntry.setCollation(op->getCollation());
    }
    deleteEntry.setHint(op->getHint());
    deleteEntry.setMulti(op->getMulti());
    deleteEntry.setQ(op->getFilter());

    std::vector<write_ops::DeleteOpEntry> deletes{deleteEntry};
    write_ops::DeleteCommandRequest deleteRequest(nsInfoEntry.getNs(), deletes);
    deleteRequest.setDollarTenant(req.getDollarTenant());
    deleteRequest.setExpectPrefix(req.getExpectPrefix());
    deleteRequest.setLet(req.getLet());
    deleteRequest.setLegacyRuntimeConstants(Variables::generateRuntimeConstants(opCtx));
    deleteRequest.getWriteCommandRequestBase().setEncryptionInformation(
        nsInfoEntry.getEncryptionInformation());
    deleteRequest.getWriteCommandRequestBase().setBypassDocumentValidation(
        req.getBypassDocumentValidation());

    return deleteRequest;
}

}  // namespace bulk_write_common
}  // namespace mongo
