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

#include "mongo/db/index_rebuilder.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/client.h"
#include "mongo/db/instance.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

    IndexRebuilder indexRebuilder;

    IndexRebuilder::IndexRebuilder() {}

    std::string IndexRebuilder::name() const {
        return "IndexRebuilder";
    }

    void IndexRebuilder::run() {
        Client::initThread(name().c_str()); 
        ON_BLOCK_EXIT_OBJ(cc(), &Client::shutdown);
        cc().getAuthorizationSession()->grantInternalAuthorization();

        std::vector<std::string> dbNames;
        getDatabaseNames(dbNames);

        try {
            std::list<std::string> collNames;
            for (std::vector<std::string>::const_iterator dbName = dbNames.begin();
                 dbName < dbNames.end();
                 dbName++) {
                Client::ReadContext ctx(*dbName);
                Database* db = ctx.ctx().db();
                db->getDatabaseCatalogEntry()->getCollectionNamespaces(&collNames);
            }
            checkNS(collNames);
        }
        catch (const DBException& e) {
            warning() << "Index rebuilding did not complete: " << e.what() << endl;
        }
        boost::unique_lock<boost::mutex> lk(replset::ReplSet::rss.mtx);
        replset::ReplSet::rss.indexRebuildDone = true;
        replset::ReplSet::rss.cond.notify_all();
        LOG(1) << "checking complete" << endl;
    }

    void IndexRebuilder::checkNS(const std::list<std::string>& nsToCheck) {
        bool firstTime = true;
        for (std::list<std::string>::const_iterator it = nsToCheck.begin();
                it != nsToCheck.end();
                ++it) {

            string ns = *it;

            LOG(3) << "IndexRebuilder::checkNS: " << ns;

            // This write lock is held throughout the index building process
            // for this namespace.
            Client::WriteContext ctx(ns);
            OperationContextImpl txn;  // XXX???

            Collection* collection = ctx.ctx().db()->getCollection( ns );
            if ( collection == NULL )
                continue;

            IndexCatalog* indexCatalog = collection->getIndexCatalog();

            if ( collection->ns().isOplog() && indexCatalog->numIndexesTotal() > 0 ) {
                warning() << ns << " had illegal indexes, removing";
                indexCatalog->dropAllIndexes(&txn, true);
                continue;
            }

            vector<BSONObj> indexesToBuild = indexCatalog->getAndClearUnfinishedIndexes(&txn);

            // The indexes have now been removed from system.indexes, so the only record is
            // in-memory. If there is a journal commit between now and when insert() rewrites
            // the entry and the db crashes before the new system.indexes entry is journalled,
            // the index will be lost forever.  Thus, we're assuming no journaling will happen
            // between now and the entry being re-written.

            if ( indexesToBuild.size() == 0 ) {
                continue;
            }

            log() << "found " << indexesToBuild.size()
                  << " interrupted index build(s) on " << ns;

            if (firstTime) {
                log() << "note: restart the server with --noIndexBuildRetry to skip index rebuilds";
                firstTime = false;
            }

            if (!serverGlobalParams.indexBuildRetry) {
                log() << "  not rebuilding interrupted indexes";
                continue;
            }

            // TODO: these can/should/must be done in parallel
            for ( size_t i = 0; i < indexesToBuild.size(); i++ ) {
                BSONObj indexObj = indexesToBuild[i];

                log() << "going to rebuild: " << indexObj;

                Status status = indexCatalog->createIndex(&txn, indexObj, false);
                if ( !status.isOK() ) {
                    log() << "building index failed: " << status.toString() << " index: " << indexObj;
                }

            }
        }
    }

}
