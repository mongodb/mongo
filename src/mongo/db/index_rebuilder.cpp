/**
 *    Copyright (C) 2012 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndexing

#include "mongo/platform/basic.h"

#include "mongo/db/index_rebuilder.h"

#include <list>
#include <string>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/client.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/instance.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {
    void checkNS(OperationContext* txn, const std::list<std::string>& nsToCheck) {
        bool firstTime = true;
        for (std::list<std::string>::const_iterator it = nsToCheck.begin();
                it != nsToCheck.end();
                ++it) {

            string ns = *it;

            LOG(3) << "IndexRebuilder::checkNS: " << ns;

            // This write lock is held throughout the index building process
            // for this namespace.
            Lock::DBLock lk(txn->lockState(), nsToDatabaseSubstring(ns), newlm::MODE_X);
            Client::Context ctx(txn, ns);

            Collection* collection = ctx.db()->getCollection(txn, ns);
            if ( collection == NULL )
                continue;

            IndexCatalog* indexCatalog = collection->getIndexCatalog();

            if ( collection->ns().isOplog() && indexCatalog->numIndexesTotal( txn ) > 0 ) {
                warning() << ns << " had illegal indexes, removing";
                indexCatalog->dropAllIndexes(txn, true);
                continue;
            }


            MultiIndexBlock indexer(txn, collection);

            {
                WriteUnitOfWork wunit(txn);
                vector<BSONObj> indexesToBuild = indexCatalog->getAndClearUnfinishedIndexes(txn);

                // The indexes have now been removed from system.indexes, so the only record is
                // in-memory. If there is a journal commit between now and when insert() rewrites
                // the entry and the db crashes before the new system.indexes entry is journalled,
                // the index will be lost forever. Thus, we must stay in the same WriteUnitOfWork
                // to ensure that no journaling will happen between now and the entry being
                // re-written in MultiIndexBlock::init(). The actual index building is done outside
                // of this WUOW.

                if (indexesToBuild.empty()) {
                    continue;
                }

                log() << "found " << indexesToBuild.size()
                      << " interrupted index build(s) on " << ns;

                if (firstTime) {
                    log() << "note: restart the server with --noIndexBuildRetry "
                          << "to skip index rebuilds";
                    firstTime = false;
                }

                if (!serverGlobalParams.indexBuildRetry) {
                    log() << "  not rebuilding interrupted indexes";
                    continue;
                }

                uassertStatusOK(indexer.init(indexesToBuild));

                wunit.commit();
            }

            try {
                uassertStatusOK(indexer.insertAllDocumentsInCollection());

                WriteUnitOfWork wunit(txn);
                indexer.commit();
                wunit.commit();
            }
            catch (...) {
                // If anything went wrong, leave the indexes partially built so that we pick them up
                // again on restart.
                indexer.abortWithoutCleanup();
                throw;
            }
        }
    }
} // namespace

    void restartInProgressIndexesFromLastShutdown(OperationContext* txn) {
        txn->getClient()->getAuthorizationSession()->grantInternalAuthorization();

        std::vector<std::string> dbNames;

        StorageEngine* storageEngine = getGlobalEnvironment()->getGlobalStorageEngine();
        storageEngine->listDatabases( &dbNames );

        try {
            std::list<std::string> collNames;
            for (std::vector<std::string>::const_iterator dbName = dbNames.begin();
                 dbName < dbNames.end();
                 ++dbName) {
                AutoGetDb autoDb(txn, *dbName, newlm::MODE_S);

                Database* db = autoDb.getDb();
                db->getDatabaseCatalogEntry()->getCollectionNamespaces(&collNames);
            }
            checkNS(txn, collNames);
        }
        catch (const DBException& e) {
            error() << "Index rebuilding did not complete: " << e.toString();
            log() << "note: restart the server with --noIndexBuildRetry to skip index rebuilds";
            fassertFailedNoTrace(18643);
        }
        LOG(1) << "checking complete" << endl;
    }
}
