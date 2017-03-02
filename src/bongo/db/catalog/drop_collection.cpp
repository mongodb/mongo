/**
 *    Copyright (C) 2015 BongoDB Inc.
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

#define BONGO_LOG_DEFAULT_COMPONENT ::bongo::logger::LogComponent::kCommand

#include "bongo/platform/basic.h"

#include "bongo/db/catalog/drop_collection.h"

#include "bongo/db/background.h"
#include "bongo/db/catalog/collection.h"
#include "bongo/db/catalog/database.h"
#include "bongo/db/catalog/index_catalog.h"
#include "bongo/db/client.h"
#include "bongo/db/concurrency/write_conflict_exception.h"
#include "bongo/db/curop.h"
#include "bongo/db/db_raii.h"
#include "bongo/db/index_builder.h"
#include "bongo/db/repl/replication_coordinator_global.h"
#include "bongo/db/server_options.h"
#include "bongo/db/service_context.h"
#include "bongo/db/views/view_catalog.h"
#include "bongo/util/log.h"

namespace bongo {

Status dropCollection(OperationContext* txn,
                      const NamespaceString& collectionName,
                      BSONObjBuilder& result) {
    if (!serverGlobalParams.quiet.load()) {
        log() << "CMD: drop " << collectionName;
    }

    const std::string dbname = collectionName.db().toString();

    BONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction transaction(txn, MODE_IX);

        AutoGetDb autoDb(txn, dbname, MODE_X);
        Database* const db = autoDb.getDb();
        Collection* coll = db ? db->getCollection(collectionName) : nullptr;
        auto view = db && !coll ? db->getViewCatalog()->lookup(txn, collectionName.ns()) : nullptr;

        if (!db || (!coll && !view)) {
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

        WriteUnitOfWork wunit(txn);
        result.append("ns", collectionName.ns());

        if (coll) {
            invariant(!view);
            int numIndexes = coll->getIndexCatalog()->numIndexesTotal(txn);

            BackgroundOperation::assertNoBgOpInProgForNs(collectionName.ns());

            Status s = db->dropCollection(txn, collectionName.ns());

            if (!s.isOK()) {
                return s;
            }

            result.append("nIndexesWas", numIndexes);
        } else {
            invariant(view);
            Status status = db->dropView(txn, collectionName.ns());
            if (!status.isOK()) {
                return status;
            }
        }
        wunit.commit();
    }
    BONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "drop", collectionName.ns());

    return Status::OK();
}

}  // namespace bongo
