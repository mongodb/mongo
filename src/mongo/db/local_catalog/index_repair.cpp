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

#include "mongo/db/local_catalog/index_repair.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace index_repair {

StatusWith<int> moveRecordToLostAndFound(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const NamespaceString& lostAndFoundNss,
                                         const RecordId& dupRecord) {
    AutoGetCollection autoColl(opCtx, lostAndFoundNss, MODE_IX);
    auto catalog = CollectionCatalog::get(opCtx);
    auto originalCollection = catalog->lookupCollectionByNamespace(opCtx, nss);

    auto localCollection = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(lostAndFoundNss,
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kWrite),
        MODE_IX);

    // Creates the collection if it doesn't exist.
    if (!localCollection.exists()) {
        Status status =
            writeConflictRetry(opCtx, "createLostAndFoundCollection", lostAndFoundNss, [&]() {
                // Ensure the database exists.
                auto db = autoColl.ensureDbExists(opCtx);
                invariant(db, lostAndFoundNss.toStringForErrorMsg());

                WriteUnitOfWork wuow(opCtx);
                ScopedLocalCatalogWriteFence scopedLocalCatalogWriteFence(opCtx, &localCollection);

                // Since we are potentially deleting a document with duplicate _id values, we need
                // to be able to insert into the lost and found collection without generating any
                // duplicate key errors on the _id value.
                CollectionOptions collOptions;
                collOptions.setNoIdIndex();
                db->createCollection(opCtx, lostAndFoundNss, collOptions);

                wuow.commit();
                return Status::OK();
            });
        if (!status.isOK()) {
            return status;
        }
    }

    // Ensure the collection exists.
    invariant(localCollection.exists(), lostAndFoundNss.toStringForErrorMsg());

    return writeConflictRetry(
        opCtx, "writeDupDocToLostAndFoundCollection", nss, [&]() -> StatusWith<int> {
            WriteUnitOfWork wuow(opCtx);
            Snapshotted<BSONObj> doc;
            int docSize = 0;

            if (!originalCollection->findDoc(opCtx, dupRecord, &doc)) {
                return docSize;
            } else {
                docSize = doc.value().objsize();
            }

            // Write document to lost_and_found collection and delete from original collection.
            Status status = collection_internal::insertDocument(
                opCtx, localCollection.getCollectionPtr(), InsertStatement(doc.value()), nullptr);
            if (!status.isOK()) {
                return status;
            }

            // CheckRecordId set to 'On' because we need _unindexKeys to confirm the record id of
            // this document matches the record id of the element it tries to unindex. This avoids
            // wrongly unindexing a document with the same _id.
            // TODO(SERVER-103399): Investigate usage validity of
            // CollectionPtr::CollectionPtr_UNSAFE
            collection_internal::deleteDocument(
                opCtx,
                CollectionPtr::CollectionPtr_UNSAFE(originalCollection),
                kUninitializedStmtId,
                dupRecord,
                nullptr /* opDebug */,
                false /* fromMigrate */,
                false /* noWarn */,
                collection_internal::StoreDeletedDoc::Off,
                CheckRecordId::On);

            wuow.commit();
            return docSize;
        });
}

int repairMissingIndexEntry(OperationContext* opCtx,
                            const IndexCatalogEntry* index,
                            const key_string::Value& ks,
                            const KeyFormat& keyFormat,
                            const NamespaceString& nss,
                            const CollectionPtr& coll,
                            ValidateResults* results) {
    auto accessMethod = const_cast<IndexAccessMethod*>(index->accessMethod())->asSortedData();
    InsertDeleteOptions options;
    options.dupsAllowed = !index->descriptor()->unique();
    int64_t numInserted = 0;
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);

    Status insertStatus = Status::OK();
    writeConflictRetry(opCtx, "insertingMissingIndexEntries", nss, [&] {
        WriteUnitOfWork wunit(opCtx);
        insertStatus =
            accessMethod->insertKeysAndUpdateMultikeyPaths(opCtx,
                                                           ru,
                                                           coll,
                                                           index,
                                                           {ks},
                                                           {},
                                                           {},
                                                           options,
                                                           nullptr,
                                                           &numInserted,
                                                           IncludeDuplicateRecordId::kOn);
        wunit.commit();
    });

    const std::string& indexName = index->descriptor()->indexName();

    // The insertKeysAndUpdateMultikeyPaths() may fail when there are missing index entries for
    // duplicate documents.
    if (numInserted > 0) {
        invariant(numInserted == 1);
        auto& indexResults = results->getIndexValidateResult(indexName);
        indexResults.addKeysTraversed(numInserted);
        results->addNumInsertedMissingIndexEntries(numInserted);
        results->setRepaired(true);
    } else {
        invariant(insertStatus.code() == ErrorCodes::DuplicateKey);

        RecordId rid = key_string::decodeRecordIdAtEnd(ks.getView(), keyFormat);

        auto dupKeyInfo = insertStatus.extraInfo<DuplicateKeyErrorInfo>();
        auto dupKeyRid = dupKeyInfo->getDuplicateRid();

        // Determine which document to remove, based on which rid is older.
        RecordId& ridToMove = rid;
        if (dupKeyRid && *dupKeyRid < rid) {
            ridToMove = *dupKeyRid;
        }

        // Move the duplicate document of the missing index entry from the record store to the lost
        // and found.
        Snapshotted<BSONObj> doc;
        if (coll->findDoc(opCtx, ridToMove, &doc)) {

            const NamespaceString lostAndFoundNss =
                NamespaceString::makeLocalCollection("lost_and_found." + coll->uuid().toString());

            auto moveStatus = moveRecordToLostAndFound(opCtx, nss, lostAndFoundNss, ridToMove);

            if (moveStatus.isOK() && (moveStatus.getValue() > 0)) {
                auto& indexResults = results->getIndexValidateResult(indexName);
                indexResults.addKeysRemovedFromRecordStore(1);
                results->addNumDocumentsMovedToLostAndFound(1);
                results->setRepaired(true);

                // If we moved the record that was already in the index, now neither of the
                // duplicate records is in the index, so we need to add the newer record to the
                // index.
                if (dupKeyRid && ridToMove == *dupKeyRid) {
                    writeConflictRetry(opCtx, "insertingMissingIndexEntries", nss, [&] {
                        WriteUnitOfWork wunit(opCtx);
                        insertStatus = accessMethod->insertKeysAndUpdateMultikeyPaths(
                            opCtx, ru, coll, index, {ks}, {}, {}, options, nullptr, nullptr);
                        wunit.commit();
                    });
                    if (!insertStatus.isOK()) {
                        results->addError(str::stream() << "unable to insert record " << rid
                                                        << " to " << indexName,
                                          false);
                    }
                }
            } else {
                results->addError(str::stream() << "unable to move record " << rid << " to "
                                                << lostAndFoundNss.toStringForErrorMsg(),
                                  false);
            }
        } else {
            // If the missing index entry does not exist in the record store, then it has
            // already been moved to the lost and found and is now outdated.
            results->addNumOutdatedMissingIndexEntry(1);
        }
    }
    return numInserted;
}

}  // namespace index_repair
}  // namespace mongo
