/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/global_index.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/transaction/retryable_writes_stats.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

namespace mongo::global_index {


void createContainer(OperationContext* opCtx, const UUID& indexUUID) {
    // StmtId will always be 0, as the command only replicates a createGlobalIndex oplog entry.
    constexpr StmtId stmtId = 0;
    if (opCtx->isRetryableWrite()) {
        const auto txnParticipant = TransactionParticipant::get(opCtx);
        if (txnParticipant.checkStatementExecuted(opCtx, stmtId)) {
            RetryableWritesStats::get(opCtx)->incrementRetriedCommandsCount();
            RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
            LOGV2_DEBUG(6789203,
                        1,
                        "_shardsvrCreateGlobalIndex retried statement already executed",
                        "indexUUID"_attr = indexUUID);
            return;
        }
    }

    const auto nss = NamespaceString::makeGlobalIndexNSS(indexUUID);
    LOGV2(6789200, "Create global index container", "indexUUID"_attr = indexUUID);

    // Create the container.
    return writeConflictRetry(opCtx, "createGlobalIndexContainer", nss.ns(), [&]() {
        const auto indexKeySpec = BSON("v" << 2 << "name"
                                           << "ik_1"
                                           << "key" << BSON("ik" << 1) << "unique" << true);

        WriteUnitOfWork wuow(opCtx);

        // createIndexesOnEmptyCollection requires the MODE_X collection lock.
        AutoGetCollection autoColl(opCtx, nss, MODE_X);
        if (!CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss)) {
            {
                repl::UnreplicatedWritesBlock unreplicatedWrites(opCtx);

                auto db = autoColl.ensureDbExists(opCtx);

                CollectionOptions collOptions;
                collOptions.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
                collOptions.uuid = indexUUID;
                db->createCollection(opCtx, nss, collOptions);

                CollectionWriter writer(opCtx, nss);
                IndexBuildsCoordinator::get(opCtx)->createIndexesOnEmptyCollection(
                    opCtx, writer, {indexKeySpec}, false);
            }
            auto opObserver = opCtx->getServiceContext()->getOpObserver();
            opObserver->onCreateGlobalIndex(opCtx, nss, indexUUID);

            wuow.commit();
        } else {
            // Container already exists, this can happen when attempting to create a global index
            // that already exists. Sanity check its storage format.
            tassert(6789204,
                    str::stream() << "Collection with UUID " << indexUUID
                                  << " already exists but it's not clustered.",
                    autoColl->getCollectionOptions().clusteredIndex);
            tassert(
                6789205,
                str::stream() << "Collection with UUID " << indexUUID
                              << " already exists but it's missing a unique index on "
                                 "'ik'.",
                autoColl->getIndexCatalog()->findIndexByKeyPatternAndOptions(
                    opCtx, BSON("ik" << 1), indexKeySpec, IndexCatalog::InclusionPolicy::kReady));
            tassert(6789206,
                    str::stream() << "Collection with namespace " << nss.ns()
                                  << " already exists but it has inconsistent UUID "
                                  << autoColl->uuid().toString() << ".",
                    autoColl->uuid() == indexUUID);
            LOGV2(6789201, "Global index container already exists", "indexUUID"_attr = indexUUID);
        }
        return;
    });
}

void insertKey(OperationContext* opCtx,
               const UUID& indexUUID,
               const BSONObj& key,
               const BSONObj& docKey) {
    const auto ns = NamespaceString::makeGlobalIndexNSS(indexUUID);

    // Generate the KeyString representation of the index key.
    // Current limitations:
    // - Only supports ascending order.
    // - We assume unique: true, and there's no support for other index options.
    // - No support for multikey indexes.

    KeyString::Builder ks(KeyString::Version::V1);
    ks.resetToKey(key, KeyString::ALL_ASCENDING);
    const auto& indexTB = ks.getTypeBits();

    // Build the index entry, consisting of:
    // - '_id': the document key.
    // - 'ik': the KeyString representation of the index key.
    // - 'tb': the index key's TypeBits. Only present if non-zero.

    BSONObjBuilder indexEntryBuilder;
    indexEntryBuilder.append("_id", docKey);
    indexEntryBuilder.append(
        "ik", BSONBinData(ks.getBuffer(), ks.getSize(), BinDataType::BinDataGeneral));
    if (!indexTB.isAllZeros()) {
        indexEntryBuilder.append(
            "tb", BSONBinData(indexTB.getBuffer(), indexTB.getSize(), BinDataType::BinDataGeneral));
    }
    const auto indexEntry = indexEntryBuilder.obj();

    // Insert the index entry.

    writeConflictRetry(opCtx, "insertGlobalIndexKey", ns.toString(), [&] {
        WriteUnitOfWork wuow(opCtx);
        AutoGetCollection container(opCtx, ns, MODE_IX);

        {
            repl::UnreplicatedWritesBlock unreplicatedWrites(opCtx);

            uassert(6789402,
                    str::stream() << "Global index container with UUID " << indexUUID
                                  << " does not exist.",
                    CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, ns));

            uassertStatusOK(collection_internal::insertDocument(
                opCtx, *container, InsertStatement(indexEntry), nullptr));
        }

        opCtx->getServiceContext()->getOpObserver()->onInsertGlobalIndexKey(
            opCtx, ns, indexUUID, key, docKey);

        wuow.commit();
    });
}

}  // namespace mongo::global_index
