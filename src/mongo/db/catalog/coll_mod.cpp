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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/coll_mod.h"

#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/service_context.h"

namespace mongo {
Status collMod(OperationContext* txn,
               const NamespaceString& nss,
               const BSONObj& cmdObj,
               BSONObjBuilder* result) {
    StringData dbName = nss.db();
    ScopedTransaction transaction(txn, MODE_IX);
    AutoGetDb autoDb(txn, dbName, MODE_X);
    Database* const db = autoDb.getDb();
    Collection* coll = db ? db->getCollection(nss) : NULL;

    // This can kill all cursors so don't allow running it while a background operation is in
    // progress.
    BackgroundOperation::assertNoBgOpInProgForNs(nss);

    // If db/collection does not exist, short circuit and return.
    if (!db || !coll) {
        return Status(ErrorCodes::NamespaceNotFound, "ns does not exist");
    }

    OldClientContext ctx(txn, nss.ns());

    bool userInitiatedWritesAndNotPrimary = txn->writesAreReplicated() &&
        !repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(nss);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::NotMaster,
                      str::stream() << "Not primary while setting collection options on "
                                    << nss.ns());
    }

    WriteUnitOfWork wunit(txn);

    Status errorStatus = Status::OK();

    BSONForEach(e, cmdObj) {
        if (str::equals("collMod", e.fieldName())) {
            // no-op
        } else if (str::startsWith(e.fieldName(), "$")) {
            // no-op ignore top-level fields prefixed with $. They are for the command processor
        } else if (QueryRequest::cmdOptionMaxTimeMS == e.fieldNameStringData()) {
            // no-op
        } else if (str::equals("index", e.fieldName())) {
            BSONObj indexObj = e.Obj();
            BSONObj keyPattern = indexObj.getObjectField("keyPattern");

            if (keyPattern.isEmpty()) {
                errorStatus = Status(ErrorCodes::InvalidOptions, "no keyPattern specified");
                continue;
            }

            BSONElement newExpireSecs = indexObj["expireAfterSeconds"];
            if (newExpireSecs.eoo()) {
                errorStatus = Status(ErrorCodes::InvalidOptions, "no expireAfterSeconds field");
                continue;
            }
            if (!newExpireSecs.isNumber()) {
                errorStatus =
                    Status(ErrorCodes::InvalidOptions, "expireAfterSeconds field must be a number");
                continue;
            }

            const IndexDescriptor* idx =
                coll->getIndexCatalog()->findIndexByKeyPattern(txn, keyPattern);
            if (idx == NULL) {
                errorStatus = Status(
                    ErrorCodes::InvalidOptions,
                    str::stream() << "cannot find index " << keyPattern << " for ns " << nss.ns());
                continue;
            }
            BSONElement oldExpireSecs = idx->infoObj().getField("expireAfterSeconds");
            if (oldExpireSecs.eoo()) {
                errorStatus =
                    Status(ErrorCodes::InvalidOptions, "no expireAfterSeconds field to update");
                continue;
            }
            if (!oldExpireSecs.isNumber()) {
                errorStatus = Status(ErrorCodes::InvalidOptions,
                                     "existing expireAfterSeconds field is not a number");
                continue;
            }

            if (oldExpireSecs != newExpireSecs) {
                result->appendAs(oldExpireSecs, "expireAfterSeconds_old");
                // Change the value of "expireAfterSeconds" on disk.
                coll->getCatalogEntry()->updateTTLSetting(
                    txn, idx->indexName(), newExpireSecs.numberLong());
                // Notify the index catalog that the definition of this index changed.
                idx = coll->getIndexCatalog()->refreshEntry(txn, idx);
                result->appendAs(newExpireSecs, "expireAfterSeconds_new");
            }
        } else if (str::equals("validator", e.fieldName())) {
            auto status = coll->setValidator(txn, e.Obj());
            if (!status.isOK())
                errorStatus = std::move(status);
        } else if (str::equals("validationLevel", e.fieldName())) {
            auto status = coll->setValidationLevel(txn, e.String());
            if (!status.isOK())
                errorStatus = std::move(status);
        } else if (str::equals("validationAction", e.fieldName())) {
            auto status = coll->setValidationAction(txn, e.String());
            if (!status.isOK())
                errorStatus = std::move(status);
        } else {
            // As of SERVER-17312 we only support these two options. When SERVER-17320 is
            // resolved this will need to be enhanced to handle other options.
            typedef CollectionOptions CO;
            const StringData name = e.fieldNameStringData();
            const int flag = (name == "usePowerOf2Sizes")
                ? CO::Flag_UsePowerOf2Sizes
                : (name == "noPadding") ? CO::Flag_NoPadding : 0;
            if (!flag) {
                errorStatus = Status(ErrorCodes::InvalidOptions,
                                     str::stream() << "unknown option to collMod: " << name);
                continue;
            }

            CollectionCatalogEntry* cce = coll->getCatalogEntry();

            const int oldFlags = cce->getCollectionOptions(txn).flags;
            const bool oldSetting = oldFlags & flag;
            const bool newSetting = e.trueValue();

            result->appendBool(name.toString() + "_old", oldSetting);
            result->appendBool(name.toString() + "_new", newSetting);

            const int newFlags = newSetting ? (oldFlags | flag)    // set flag
                                            : (oldFlags & ~flag);  // clear flag

            // NOTE we do this unconditionally to ensure that we note that the user has
            // explicitly set flags, even if they are just setting the default.
            cce->updateFlags(txn, newFlags);

            const CollectionOptions newOptions = cce->getCollectionOptions(txn);
            invariant(newOptions.flags == newFlags);
            invariant(newOptions.flagsSet);
        }
    }

    if (!errorStatus.isOK()) {
        return errorStatus;
    }

    getGlobalServiceContext()->getOpObserver()->onCollMod(
        txn, (dbName.toString() + ".$cmd").c_str(), cmdObj);

    wunit.commit();
    return Status::OK();
}
}  // namespace mongo
