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

struct CollModRequest {
    const IndexDescriptor* idx = nullptr;
    BSONElement indexExpireAfterSeconds = {};
    BSONElement collValidator = {};
    std::string collValidationAction = {};
    std::string collValidationLevel = {};
    BSONElement usePowerOf2Sizes = {};
    BSONElement noPadding = {};
};

StatusWith<CollModRequest> parseCollModRequest(OperationContext* txn,
                                               const NamespaceString& nss,
                                               Collection* coll,
                                               const BSONObj& cmdObj) {
    CollModRequest cmr;

    BSONForEach(e, cmdObj) {
        if (str::equals("collMod", e.fieldName())) {
            // no-op
        } else if (str::startsWith(e.fieldName(), "$")) {
            // no-op ignore top-level fields prefixed with $. They are for the command processor
        } else if (LiteParsedQuery::cmdOptionMaxTimeMS == e.fieldNameStringData()) {
            // no-op
        } else if (str::equals("index", e.fieldName())) {
            BSONObj indexObj = e.Obj();
            BSONObj keyPattern = indexObj.getObjectField("keyPattern");

            if (keyPattern.isEmpty()) {
                return Status(ErrorCodes::InvalidOptions, "no keyPattern specified");
            }
            cmr.indexExpireAfterSeconds = indexObj["expireAfterSeconds"];
            if (cmr.indexExpireAfterSeconds.eoo()) {
                return Status(ErrorCodes::InvalidOptions, "no expireAfterSeconds field");
            }
            if (!cmr.indexExpireAfterSeconds.isNumber()) {
                return Status(ErrorCodes::InvalidOptions,
                              "expireAfterSeconds field must be a number");
            }

            const IndexDescriptor* idx =
                coll->getIndexCatalog()->findIndexByKeyPattern(txn, keyPattern);
            if (idx == NULL) {
                return Status(ErrorCodes::IndexNotFound,
                              str::stream() << "cannot find index " << keyPattern << " for ns "
                                            << nss.ns());
            }
            cmr.idx = idx;

            BSONElement oldExpireSecs = cmr.idx->infoObj().getField("expireAfterSeconds");

            if (oldExpireSecs.eoo()) {
                return Status(ErrorCodes::InvalidOptions, "no expireAfterSeconds field to update");
            }
            if (!oldExpireSecs.isNumber()) {
                return Status(ErrorCodes::InvalidOptions,
                              "existing expireAfterSeconds field is not a number");
            }
        } else if (str::equals("validator", e.fieldName())) {
            auto statusW = coll->parseValidator(e.Obj());
            if (!statusW.isOK())
                return statusW.getStatus();

            cmr.collValidator = e;
        } else if (str::equals("validationLevel", e.fieldName())) {
            auto statusW = coll->parseValidationLevel(e.String());
            if (!statusW.isOK())
                return statusW.getStatus();

            cmr.collValidationLevel = e.String();
        } else if (str::equals("validationAction", e.fieldName())) {
            auto statusW = coll->parseValidationAction(e.String());
            if (!statusW.isOK())
                return statusW.getStatus();

            cmr.collValidationAction = e.String();
        } else {
            // As of SERVER-17312 we only support these two options. When SERVER-17320 is
            // resolved this will need to be enhanced to handle other options.
            const StringData name = e.fieldNameStringData();
            if (name == "usePowerOf2Sizes")
                cmr.usePowerOf2Sizes = e;
            else if (name == "noPadding")
                cmr.noPadding = e;
            else
                return Status(ErrorCodes::InvalidOptions,
                              str::stream() << "unknown option to collMod: " << name);
        }
    }

    return {std::move(cmr)};
}

Status collMod(OperationContext* txn,
               const NamespaceString& nss,
               const BSONObj& cmdObj,
               BSONObjBuilder* result) {
    StringData dbName = nss.db();
    ScopedTransaction transaction(txn, MODE_IX);
    AutoGetDb autoDb(txn, dbName, MODE_X);
    Database* const db = autoDb.getDb();
    Collection* coll = db ? db->getCollection(nss) : nullptr;

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

    auto statusW = parseCollModRequest(txn, nss, coll, cmdObj);
    if (!statusW.isOK()) {
        return statusW.getStatus();
    }

    CollModRequest cmr = statusW.getValue();

    WriteUnitOfWork wunit(txn);

    if (!cmr.indexExpireAfterSeconds.eoo()) {
        BSONElement& newExpireSecs = cmr.indexExpireAfterSeconds;
        BSONElement oldExpireSecs = cmr.idx->infoObj().getField("expireAfterSeconds");

        if (oldExpireSecs != newExpireSecs) {
            result->appendAs(oldExpireSecs, "expireAfterSeconds_old");
            // Change the value of "expireAfterSeconds" on disk.
            coll->getCatalogEntry()->updateTTLSetting(
                txn, cmr.idx->indexName(), newExpireSecs.safeNumberLong());
            // Notify the index catalog that the definition of this index changed.
            cmr.idx = coll->getIndexCatalog()->refreshEntry(txn, cmr.idx);
            result->appendAs(newExpireSecs, "expireAfterSeconds_new");
        }
    }

    if (!cmr.collValidator.eoo())
        coll->setValidator(txn, cmr.collValidator.Obj());

    if (!cmr.collValidationAction.empty())
        coll->setValidationAction(txn, cmr.collValidationAction);

    if (!cmr.collValidationLevel.empty())
        coll->setValidationLevel(txn, cmr.collValidationLevel);

    auto setCollectionOption = [&](BSONElement& COElement) {
        typedef CollectionOptions CO;
        const StringData name = COElement.fieldNameStringData();

        int flag = (name == "usePowerOf2Sizes") ? CO::Flag_UsePowerOf2Sizes
                                                : (name == "noPadding") ? CO::Flag_NoPadding : 0;

        CollectionCatalogEntry* cce = coll->getCatalogEntry();

        const int oldFlags = cce->getCollectionOptions(txn).flags;
        const bool oldSetting = oldFlags & flag;
        const bool newSetting = COElement.trueValue();

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
    };

    if (!cmr.usePowerOf2Sizes.eoo()) {
        setCollectionOption(cmr.usePowerOf2Sizes);
    }

    if (!cmr.noPadding.eoo()) {
        setCollectionOption(cmr.noPadding);
    }

    getGlobalServiceContext()->getOpObserver()->onCollMod(
        txn, (dbName.toString() + ".$cmd").c_str(), cmdObj);

    wunit.commit();

    return Status::OK();
}
}  // namespace mongo
