/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/pipeline/spilling/spilling_test_process_interface.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"

namespace mongo {

std::unique_ptr<SpillTable> SpillingTestMongoProcessInterface::createSpillTable(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, KeyFormat keyFormat) const {
    shard_role_details::getRecoveryUnit(expCtx->getOperationContext())->abandonSnapshot();
    shard_role_details::getRecoveryUnit(expCtx->getOperationContext())
        ->setPrepareConflictBehavior(PrepareConflictBehavior::kIgnoreConflictsAllowWrites);
    auto storageEngine = expCtx->getOperationContext()->getServiceContext()->getStorageEngine();
    return storageEngine->makeTemporaryRecordStore(
        expCtx->getOperationContext(), storageEngine->generateNewInternalIdent(), keyFormat);
}

void SpillingTestMongoProcessInterface::writeRecordsToSpillTable(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    SpillTable& spillTable,
    std::vector<Record>* records) const {

    writeConflictRetry(
        expCtx->getOperationContext(),
        "SpillingTestMongoProcessInterface::writeRecordsToSpillTable",
        expCtx->getNamespaceString(),
        [&] {
            AutoGetCollection autoColl(
                expCtx->getOperationContext(), expCtx->getNamespaceString(), MODE_IS);
            WriteUnitOfWork wuow(expCtx->getOperationContext());
            auto writeResult = spillTable.insertRecords(expCtx->getOperationContext(), records);
            tassert(5643014,
                    str::stream() << "Failed to write to disk because " << writeResult.reason(),
                    writeResult.isOK());
            wuow.commit();
        });
}

Document SpillingTestMongoProcessInterface::readRecordFromSpillTable(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const SpillTable& spillTable,
    RecordId rID) const {
    RecordData possibleRecord;
    AutoGetCollection autoColl(
        expCtx->getOperationContext(), expCtx->getNamespaceString(), MODE_IS);
    auto foundDoc =
        spillTable.findRecord(expCtx->getOperationContext(), RecordId(rID), &possibleRecord);
    tassert(5643001, str::stream() << "Could not find document id " << rID, foundDoc);
    return Document::fromBsonWithMetaData(possibleRecord.toBson());
}

bool SpillingTestMongoProcessInterface::checkRecordInSpillTable(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const SpillTable& spillTable,
    RecordId rID) const {
    RecordData possibleRecord;
    AutoGetCollection autoColl(
        expCtx->getOperationContext(), expCtx->getNamespaceString(), MODE_IS);
    return spillTable.findRecord(expCtx->getOperationContext(), RecordId(rID), &possibleRecord);
}

void SpillingTestMongoProcessInterface::deleteRecordFromSpillTable(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    SpillTable& spillTable,
    RecordId rID) const {
    AutoGetCollection autoColl(
        expCtx->getOperationContext(), expCtx->getNamespaceString(), MODE_IS);
    WriteUnitOfWork wuow(expCtx->getOperationContext());
    spillTable.deleteRecord(expCtx->getOperationContext(), rID);
    wuow.commit();
}

void SpillingTestMongoProcessInterface::truncateSpillTable(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, SpillTable& spillTable) const {
    AutoGetCollection autoColl(
        expCtx->getOperationContext(), expCtx->getNamespaceString(), MODE_IS);
    WriteUnitOfWork wuow(expCtx->getOperationContext());
    auto status = spillTable.truncate(expCtx->getOperationContext());
    tassert(5643015, "Unable to clear record store", status.isOK());
    wuow.commit();
}

}  // namespace mongo
