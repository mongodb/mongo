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

#include <boost/optional.hpp>
#include <memory>

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/views/view_catalog.h"

namespace mongo {

struct CollModRequest {
    const IndexDescriptor* idx = nullptr;
    BSONElement indexExpireAfterSeconds = {};
    BSONElement viewPipeLine = {};
    std::string viewOn = {};
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

    bool isView = !coll;

    CollModRequest cmr;

    BSONForEach(e, cmdObj) {
        if (str::equals("collMod", e.fieldName())) {
            // no-op
        } else if (str::startsWith(e.fieldName(), "$")) {
            // no-op ignore top-level fields prefixed with $. They are for the command processor
        } else if (QueryRequest::cmdOptionMaxTimeMS == e.fieldNameStringData()) {
            // no-op
        } else if (str::equals("index", e.fieldName()) && !isView) {
            BSONObj indexObj = e.Obj();
            StringData indexName;
            BSONObj keyPattern;

            BSONElement nameElem = indexObj["name"];
            BSONElement keyPatternElem = indexObj["keyPattern"];
            if (nameElem && keyPatternElem) {
                return Status(ErrorCodes::InvalidOptions,
                              "Cannot specify both key pattern and name.");
            }

            if (!nameElem && !keyPatternElem) {
                return Status(ErrorCodes::InvalidOptions,
                              "Must specify either index name or key pattern.");
            }

            if (nameElem) {
                if (nameElem.type() != BSONType::String) {
                    return Status(ErrorCodes::InvalidOptions, "Index name must be a string.");
                }
                indexName = nameElem.valueStringData();
            }

            if (keyPatternElem) {
                if (keyPatternElem.type() != BSONType::Object) {
                    return Status(ErrorCodes::InvalidOptions, "Key pattern must be an object.");
                }
                keyPattern = keyPatternElem.embeddedObject();
            }

            cmr.indexExpireAfterSeconds = indexObj["expireAfterSeconds"];
            if (cmr.indexExpireAfterSeconds.eoo()) {
                return Status(ErrorCodes::InvalidOptions, "no expireAfterSeconds field");
            }
            if (!cmr.indexExpireAfterSeconds.isNumber()) {
                return Status(ErrorCodes::InvalidOptions,
                              "expireAfterSeconds field must be a number");
            }

            if (!indexName.empty()) {
                cmr.idx = coll->getIndexCatalog()->findIndexByName(txn, indexName);
                if (!cmr.idx) {
                    return Status(ErrorCodes::IndexNotFound,
                                  str::stream() << "cannot find index " << indexName << " for ns "
                                                << nss.ns());
                }
            } else {
                std::vector<IndexDescriptor*> indexes;
                coll->getIndexCatalog()->findIndexesByKeyPattern(txn, keyPattern, false, &indexes);

                if (indexes.size() > 1) {
                    return Status(ErrorCodes::AmbiguousIndexKeyPattern,
                                  str::stream() << "index keyPattern " << keyPattern << " matches "
                                                << indexes.size()
                                                << " indexes,"
                                                << " must use index name. "
                                                << "Conflicting indexes:"
                                                << indexes[0]->infoObj()
                                                << ", "
                                                << indexes[1]->infoObj());
                } else if (indexes.empty()) {
                    return Status(ErrorCodes::IndexNotFound,
                                  str::stream() << "cannot find index " << keyPattern << " for ns "
                                                << nss.ns());
                }

                cmr.idx = indexes[0];
            }

            BSONElement oldExpireSecs = cmr.idx->infoObj().getField("expireAfterSeconds");
            if (oldExpireSecs.eoo()) {
                return Status(ErrorCodes::InvalidOptions, "no expireAfterSeconds field to update");
            }
            if (!oldExpireSecs.isNumber()) {
                return Status(ErrorCodes::InvalidOptions,
                              "existing expireAfterSeconds field is not a number");
            }

        } else if (str::equals("validator", e.fieldName()) && !isView) {
            auto statusW = coll->parseValidator(e.Obj());
            if (!statusW.isOK())
                return statusW.getStatus();

            cmr.collValidator = e;
        } else if (str::equals("validationLevel", e.fieldName()) && !isView) {
            auto statusW = coll->parseValidationLevel(e.String());
            if (!statusW.isOK())
                return statusW.getStatus();

            cmr.collValidationLevel = e.String();
        } else if (str::equals("validationAction", e.fieldName()) && !isView) {
            auto statusW = coll->parseValidationAction(e.String());
            if (!statusW.isOK())
                statusW.getStatus();

            cmr.collValidationAction = e.String();
        } else if (str::equals("pipeline", e.fieldName())) {
            if (!isView) {
                return Status(ErrorCodes::InvalidOptions,
                              "'pipeline' option only supported on a view");
            }
            if (e.type() != mongo::Array) {
                return Status(ErrorCodes::InvalidOptions, "not a valid aggregation pipeline");
            }
            cmr.viewPipeLine = e;
        } else if (str::equals("viewOn", e.fieldName())) {
            if (!isView) {
                return Status(ErrorCodes::InvalidOptions,
                              "'viewOn' option only supported on a view");
            }
            if (e.type() != mongo::String) {
                return Status(ErrorCodes::InvalidOptions, "'viewOn' option must be a string");
            }
            cmr.viewOn = e.str();
        } else {
            const StringData name = e.fieldNameStringData();
            if (isView) {
                return Status(ErrorCodes::InvalidOptions,
                              str::stream() << "option not supported on a view: " << name);
            }
            // As of SERVER-17312 we only support these two options. When SERVER-17320 is
            // resolved this will need to be enhanced to handle other options.
            typedef CollectionOptions CO;

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

    // May also modify a view instead of a collection.
    boost::optional<ViewDefinition> view;
    if (db && !coll) {
        const auto sharedView = db->getViewCatalog()->lookup(txn, nss.ns());
        if (sharedView) {
            // We copy the ViewDefinition as it is modified below to represent the requested state.
            view = {*sharedView};
        }
    }

    // This can kill all cursors so don't allow running it while a background operation is in
    // progress.
    BackgroundOperation::assertNoBgOpInProgForNs(nss);

    // If db/collection/view does not exist, short circuit and return.
    if (!db || (!coll && !view)) {
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

    if (view) {
        if (!cmr.viewPipeLine.eoo())
            view->setPipeline(cmr.viewPipeLine);

        if (!cmr.viewOn.empty())
            view->setViewOn(NamespaceString(dbName, cmr.viewOn));

        ViewCatalog* catalog = db->getViewCatalog();

        BSONArrayBuilder pipeline;
        for (auto& item : view->pipeline()) {
            pipeline.append(item);
        }
        auto errorStatus = catalog->modifyView(txn, nss, view->viewOn(), BSONArray(pipeline.obj()));
        if (!errorStatus.isOK()) {
            return errorStatus;
        }
    } else {
        if (!cmr.indexExpireAfterSeconds.eoo()) {
            BSONElement& newExpireSecs = cmr.indexExpireAfterSeconds;
            BSONElement oldExpireSecs = cmr.idx->infoObj().getField("expireAfterSeconds");

            if (SimpleBSONElementComparator::kInstance.evaluate(oldExpireSecs != newExpireSecs)) {
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

            int flag = (name == "usePowerOf2Sizes")
                ? CO::Flag_UsePowerOf2Sizes
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

        // Only observe non-view collMods, as view operations are observed as operations on the
        // system.views collection.
        getGlobalServiceContext()->getOpObserver()->onCollMod(
            txn, (dbName.toString() + ".$cmd").c_str(), cmdObj);
    }

    wunit.commit();

    return Status::OK();
}
}  // namespace mongo
