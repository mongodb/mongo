/**
*    Copyright (C) 2014 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/repair_database.h"

#include <boost/scoped_ptr.hpp>

#include "mongo/db/background.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_engine.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/log.h"

namespace mongo {

    using std::endl;
    using std::string;

namespace {
    Status rebuildIndexesOnCollection(OperationContext* txn,
                                      DatabaseCatalogEntry* dbce,
                                      const std::string& collectionName) {

        CollectionCatalogEntry* cce = dbce->getCollectionCatalogEntry(collectionName);

        std::vector<string> indexNames;
        std::vector<BSONObj> indexSpecs;
        {
            // Fetch all indexes
            cce->getAllIndexes( txn, &indexNames );
            for ( size_t i = 0; i < indexNames.size(); i++ ) {
                const string& name = indexNames[i];
                BSONObj spec = cce->getIndexSpec( txn, name );
                indexSpecs.push_back(spec.removeField("v").getOwned());

                const BSONObj key = spec.getObjectField("key");
                const Status keyStatus = validateKeyPattern(key);
                if (!keyStatus.isOK()) {
                    return Status(ErrorCodes::CannotCreateIndex, str::stream()
                        << "Cannot rebuild index " << spec << ": " << keyStatus.reason()
                        << " For more info see http://dochub.mongodb.org/core/index-validation");
                }
            }
        }

        // Skip the rest if there are no indexes to rebuild.
        if (indexSpecs.empty()) return Status::OK();

        boost::scoped_ptr<Collection> collection;
        boost::scoped_ptr<MultiIndexBlock> indexer;
        {
            // These steps are combined into a single WUOW to ensure there are no commits without
            // the indexes.
            // 1) Drop all indexes.
            // 2) Open the Collection
            // 3) Start the index build process.

            WriteUnitOfWork wuow(txn);

            { // 1
                for ( size_t i = 0; i < indexNames.size(); i++ ) {
                    Status s = cce->removeIndex(txn, indexNames[i]);
                    if (!s.isOK()) return s;
                }
            }

            // Indexes must be dropped before we open the Collection otherwise we could attempt to
            // open a bad index and fail.
            // TODO see if MultiIndexBlock can be made to work without a Collection.
            const StringData ns = cce->ns().ns();
            collection.reset(new Collection(txn, ns, cce, dbce->getRecordStore(ns), dbce));

            indexer.reset(new MultiIndexBlock(txn, collection.get()));
            Status status = indexer->init(indexSpecs);
            if (!status.isOK()) {
                // The WUOW will handle cleanup, so the indexer shouldn't do its own.
                indexer->abortWithoutCleanup();
                return status;
            }

            wuow.commit();
        }

        // Iterate all records in the collection. Delete them if they aren't valid BSON. Index them
        // if they are.

        long long numRecords = 0;
        long long dataSize = 0;

        RecordStore* rs = collection->getRecordStore();
        boost::scoped_ptr<RecordIterator> it(rs->getIterator(txn));
        while (!it->isEOF()) {
            RecordId id = it->curr();
            RecordData data = it->dataFor(id);
            invariant(id == it->getNext());

            Status status = validateBSON(data.data(), data.size());
            if (!status.isOK()) {
                log() << "Invalid BSON detected at " << id << ": " << status << ". Deleting.";
                it->saveState();
                {
                    WriteUnitOfWork wunit(txn);
                    rs->deleteRecord(txn, id);
                    wunit.commit();
                }
                it->restoreState(txn);
                continue;
            }

            numRecords++;
            dataSize += data.size();

            // Now index the record.
            // TODO SERVER-14812 add a mode that drops duplicates rather than failing
            WriteUnitOfWork wunit(txn);
            status = indexer->insert(data.releaseToBson(), id);
            if (!status.isOK()) return status;
            wunit.commit();
        }

        Status status = indexer->doneInserting();
        if (!status.isOK()) return status;

        {
            WriteUnitOfWork wunit(txn);
            indexer->commit();
            rs->updateStatsAfterRepair(txn, numRecords, dataSize);
            wunit.commit();
        }

        return Status::OK();
    }
} // namespace

    Status repairDatabase(OperationContext* txn,
                          StorageEngine* engine,
                          const std::string& dbName,
                          bool preserveClonedFilesOnFailure,
                          bool backupOriginalFiles) {



        // We must hold some form of lock here
        invariant(txn->lockState()->isLocked());
        invariant( dbName.find( '.' ) == string::npos );

        log() << "repairDatabase " << dbName << endl;

        BackgroundOperation::assertNoBgOpInProgForDb(dbName);

        txn->checkForInterrupt();

        if (engine->isMmapV1()) {
            // MMAPv1 is a layering violation so it implements its own repairDatabase.
            return static_cast<MMAPV1Engine*>(engine)->repairDatabase(txn,
                                                                      dbName,
                                                                      preserveClonedFilesOnFailure,
                                                                      backupOriginalFiles);
        }

        // These are MMAPv1 specific
        if ( preserveClonedFilesOnFailure ) {
            return Status( ErrorCodes::BadValue, "preserveClonedFilesOnFailure not supported" );
        }
        if ( backupOriginalFiles ) {
            return Status( ErrorCodes::BadValue, "backupOriginalFiles not supported" );
        }

        // Close the db to invalidate all current users and caches.
        dbHolder().close(txn, dbName);
        // Open the db after everything finishes
        class OpenDbInDestructor {
        public:
            OpenDbInDestructor(OperationContext* txn, const std::string& db) :
                  _dbName(db)
                , _txn(txn)
            {}
            ~OpenDbInDestructor() {
                dbHolder().openDb(_txn, _dbName);
            }
        private:
            const std::string& _dbName;
            OperationContext* _txn;
        } dbOpener(txn, dbName);
        DatabaseCatalogEntry* dbce = engine->getDatabaseCatalogEntry(txn, dbName);

        std::list<std::string> colls;
        dbce->getCollectionNamespaces(&colls);

        for (std::list<std::string>::const_iterator it = colls.begin(); it != colls.end(); ++it) {
            // Don't check for interrupt after starting to repair a collection otherwise we can
            // leave data in an inconsistent state. Interrupting between collections is ok, however.
            txn->checkForInterrupt();

            log() << "Repairing collection " << *it;

            Status status = engine->repairRecordStore(txn, *it);
            if (!status.isOK()) return status;

            status = rebuildIndexesOnCollection(txn, dbce, *it);
            if (!status.isOK()) return status;

            // TODO: uncomment once SERVER-16869
            // engine->flushAllFiles(true);
        }

        return Status::OK();
    }
}

