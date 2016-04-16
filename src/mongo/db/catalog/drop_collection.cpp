/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/drop_collection.h"

#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"

namespace mongo {

Status dropCollection(OperationContext* txn,
                      const NamespaceString& collectionName,
                      BSONObjBuilder& result) {
    if (!serverGlobalParams.quiet) {
        log() << "CMD: drop " << collectionName;
    }

    const std::string dbname = collectionName.db().toString();

    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IX);

        AutoGetDb autoDb(txn, dbname, MODE_X);
        Database* const db = autoDb.getDb();
        Collection* coll = db ? db->getCollection(collectionName) : nullptr;

        // If db/collection does not exist, short circuit and return.
        if (!db || !coll) {
            return Status(ErrorCodes::NamespaceNotFound, "ns not found");
        }

        const bool shardVersionCheck = true;
        OldClientContext context(txn, collectionName.ns(), shardVersionCheck);

        bool userInitiatedWritesAndNotPrimary = txn->writesAreReplicated() &&
            !repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(collectionName);

        if (userInitiatedWritesAndNotPrimary) {
            return Status(ErrorCodes::NotMaster,
                          str::stream() << "Not primary while dropping collection "
                                        << collectionName.ns());
        }

        int numIndexes = coll->getIndexCatalog()->numIndexesTotal(txn);

        BackgroundOperation::assertNoBgOpInProgForNs(collectionName.ns());

        WriteUnitOfWork wunit(txn);
        Status s = db->dropCollection(txn, collectionName.ns());

        result.append("ns", collectionName.ns());

        if (!s.isOK()) {
            return s;
        }

        result.append("nIndexesWas", numIndexes);

        wunit.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "drop", collectionName.ns());

    return Status::OK();
}

}  // namespace mongo
