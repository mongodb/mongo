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

#include "mongo/db/timeseries/timeseries_write_util.h"

#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/curop.h"
#include "mongo/db/ops/write_ops_exec_util.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"

namespace mongo::timeseries {

Status performAtomicWrites(
    OperationContext* opCtx,
    const CollectionPtr& coll,
    const RecordId& recordId,
    const stdx::variant<write_ops::UpdateCommandRequest, write_ops::DeleteCommandRequest>&
        modificationOp) try {
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());
    invariant(!opCtx->inMultiDocumentTransaction());

    NamespaceString ns = coll->ns();

    DisableDocumentValidation disableDocumentValidation{opCtx};

    write_ops_exec::LastOpFixer lastOpFixer{opCtx, ns};
    lastOpFixer.startingOp();

    auto curOp = CurOp::get(opCtx);
    curOp->raiseDbProfileLevel(CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(ns.dbName()));

    write_ops_exec::assertCanWrite_inlock(opCtx, ns);

    WriteUnitOfWork wuow{opCtx};

    stdx::visit(
        OverloadedVisitor{
            [&](const write_ops::UpdateCommandRequest& updateOp) {
                invariant(updateOp.getUpdates().size() == 1);
                auto& update = updateOp.getUpdates().front();

                invariant(coll->isClustered());

                auto original = coll->docFor(opCtx, recordId);

                CollectionUpdateArgs args{original.value()};
                args.criteria = update.getQ();
                if (const auto& stmtIds = updateOp.getStmtIds()) {
                    args.stmtIds = *stmtIds;
                }
                args.source = OperationSource::kTimeseriesDelete;

                BSONObj diffFromUpdate;
                const BSONObj* diffOnIndexes =
                    collection_internal::kUpdateAllIndexes;  // Assume all indexes are affected.

                // Overwrites the original bucket.
                invariant(update.getU().type() ==
                          write_ops::UpdateModification::Type::kReplacement);
                auto updated = update.getU().getUpdateReplacement();
                args.update = update_oplog_entry::makeReplacementOplogEntry(updated);

                collection_internal::updateDocument(opCtx,
                                                    coll,
                                                    recordId,
                                                    original,
                                                    updated,
                                                    diffOnIndexes,
                                                    &curOp->debug(),
                                                    &args);
            },
            [&](const write_ops::DeleteCommandRequest& deleteOp) {
                invariant(deleteOp.getDeletes().size() == 1);
                auto deleteId =
                    record_id_helpers::keyForOID(deleteOp.getDeletes().front().getQ()["_id"].OID());
                invariant(recordId == deleteId);
                collection_internal::deleteDocument(
                    opCtx, coll, kUninitializedStmtId, recordId, &curOp->debug());
            }},
        modificationOp);

    wuow.commit();

    lastOpFixer.finishedOpSuccessfully();

    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

}  // namespace mongo::timeseries
