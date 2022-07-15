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

#include "mongo/db/catalog/index_repair.h"
#include "mongo/base/status_with.h"
#include "mongo/db/catalog/validate_state.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/logv2/log_debug.h"

namespace mongo {
namespace index_repair {

StatusWith<int> moveRecordToLostAndFound(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const NamespaceString& lostAndFoundNss,
                                         const RecordId& dupRecord) {
    AutoGetCollection autoColl(opCtx, lostAndFoundNss, MODE_IX);
    auto catalog = CollectionCatalog::get(opCtx);
    auto originalCollection = catalog->lookupCollectionByNamespace(opCtx, nss);
    CollectionPtr localCollection = catalog->lookupCollectionByNamespace(opCtx, lostAndFoundNss);

    // Creates the collection if it doesn't exist.
    if (!localCollection) {
        Status status =
            writeConflictRetry(opCtx, "createLostAndFoundCollection", lostAndFoundNss.ns(), [&]() {
                // Ensure the database exists.
                auto db = autoColl.ensureDbExists(opCtx);
                invariant(db, lostAndFoundNss.ns());

                WriteUnitOfWork wuow(opCtx);

                // Since we are potentially deleting a document with duplicate _id values, we need
                // to be able to insert into the lost and found collection without generating any
                // duplicate key errors on the _id value.
                CollectionOptions collOptions;
                collOptions.setNoIdIndex();
                localCollection = db->createCollection(opCtx, lostAndFoundNss, collOptions);

                // Ensure the collection exists.
                invariant(localCollection, lostAndFoundNss.ns());

                wuow.commit();
                return Status::OK();
            });
        if (!status.isOK()) {
            return status;
        }
    }

    return writeConflictRetry(
        opCtx, "writeDupDocToLostAndFoundCollection", nss.ns(), [&]() -> StatusWith<int> {
            WriteUnitOfWork wuow(opCtx);
            Snapshotted<BSONObj> doc;
            int docSize = 0;

            if (!originalCollection->findDoc(opCtx, dupRecord, &doc)) {
                return docSize;
            } else {
                docSize = doc.value().objsize();
            }

            // Write document to lost_and_found collection and delete from original collection.
            Status status =
                localCollection->insertDocument(opCtx, InsertStatement(doc.value()), nullptr);
            if (!status.isOK()) {
                return status;
            }

            // CheckRecordId set to 'On' because we need _unindexKeys to confirm the record id of
            // this document matches the record id of the element it tries to unindex. This avoids
            // wrongly unindexing a document with the same _id.
            originalCollection->deleteDocument(opCtx,
                                               kUninitializedStmtId,
                                               dupRecord,
                                               nullptr /* opDebug */,
                                               false /* fromMigrate */,
                                               false /* noWarn */,
                                               Collection::StoreDeletedDoc::Off,
                                               CheckRecordId::On);

            wuow.commit();
            return docSize;
        });
}

int repairMissingIndexEntry(OperationContext* opCtx,
                            std::shared_ptr<const IndexCatalogEntry>& index,
                            const KeyString::Value& ks,
                            const KeyFormat& keyFormat,
                            const NamespaceString& nss,
                            const CollectionPtr& coll,
                            ValidateResults* results) {
    auto accessMethod = const_cast<IndexAccessMethod*>(index->accessMethod())->asSortedData();
    InsertDeleteOptions options;
    options.dupsAllowed = !index->descriptor()->unique();
    int64_t numInserted = 0;

    writeConflictRetry(opCtx, "insertingMissingIndexEntries", nss.ns(), [&] {
        WriteUnitOfWork wunit(opCtx);
        // Ignore return status because we will use numInserted to verify success.
        accessMethod
            ->insertKeysAndUpdateMultikeyPaths(
                opCtx, coll, {ks}, {}, {}, options, nullptr, &numInserted)
            .ignore();
        wunit.commit();
    });

    const std::string& indexName = index->descriptor()->indexName();

    // The insertKeysAndUpdateMultikeyPaths() may fail when there are missing index entries for
    // duplicate documents.
    if (numInserted > 0) {
        auto& indexResults = results->indexResultsMap[indexName];
        indexResults.keysTraversed += numInserted;
        results->numInsertedMissingIndexEntries += numInserted;
        results->repaired = true;
    } else {
        RecordId rid;
        if (keyFormat == KeyFormat::Long) {
            rid = KeyString::decodeRecordIdLongAtEnd(ks.getBuffer(), ks.getSize());
        } else {
            invariant(keyFormat == KeyFormat::String);
            rid = KeyString::decodeRecordIdStrAtEnd(ks.getBuffer(), ks.getSize());
        }

        // Move the duplicate document of the missing index entry from the record store to the lost
        // and found.
        Snapshotted<BSONObj> doc;
        if (coll->findDoc(opCtx, rid, &doc)) {
            const NamespaceString lostAndFoundNss = NamespaceString(
                NamespaceString::kLocalDb, "lost_and_found." + coll->uuid().toString());

            auto moveStatus = moveRecordToLostAndFound(opCtx, nss, lostAndFoundNss, rid);
            if (moveStatus.isOK() && (moveStatus.getValue() > 0)) {
                auto& indexResults = results->indexResultsMap[indexName];
                indexResults.keysRemovedFromRecordStore++;
                results->numDocumentsMovedToLostAndFound++;
                results->repaired = true;
            } else {
                results->errors.push_back(str::stream() << "unable to move record " << rid << " to "
                                                        << lostAndFoundNss.ns());
            }
        } else {
            // If the missing index entry does not exist in the record store, then it has
            // already been moved to the lost and found and is now outdated.
            results->numOutdatedMissingIndexEntry++;
        }
    }
    return numInserted;
}

}  // namespace index_repair
}  // namespace mongo
