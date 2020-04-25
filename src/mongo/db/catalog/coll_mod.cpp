/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/coll_mod.h"

#include <boost/optional.hpp>
#include <memory>

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/command_generic_argument.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(hangAfterDatabaseLock);
MONGO_FAIL_POINT_DEFINE(assertAfterIndexUpdate);

struct CollModRequest {
    const IndexDescriptor* idx = nullptr;
    BSONElement indexExpireAfterSeconds = {};
    BSONElement indexHidden = {};
    BSONElement viewPipeLine = {};
    std::string viewOn = {};
    BSONElement collValidator = {};
    std::string collValidationAction = {};
    std::string collValidationLevel = {};
    bool recordPreImages = false;
};

StatusWith<CollModRequest> parseCollModRequest(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               Collection* coll,
                                               const BSONObj& cmdObj,
                                               BSONObjBuilder* oplogEntryBuilder) {

    bool isView = !coll;

    CollModRequest cmr;

    BSONForEach(e, cmdObj) {
        const auto fieldName = e.fieldNameStringData();
        if (isGenericArgument(fieldName)) {
            continue;  // Don't add to oplog builder.
        } else if (fieldName == "collMod") {
            // no-op
        } else if (fieldName == "index" && !isView) {
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
            cmr.indexHidden = indexObj["hidden"];

            if (cmr.indexExpireAfterSeconds.eoo() && cmr.indexHidden.eoo()) {
                return Status(ErrorCodes::InvalidOptions, "no expireAfterSeconds or hidden field");
            }
            if (!cmr.indexExpireAfterSeconds.eoo() && !cmr.indexExpireAfterSeconds.isNumber()) {
                return Status(ErrorCodes::InvalidOptions,
                              "expireAfterSeconds field must be a number");
            }
            if (!cmr.indexHidden.eoo() && !cmr.indexHidden.isBoolean()) {
                return Status(ErrorCodes::InvalidOptions, "hidden field must be a boolean");
            }
            if (!indexName.empty()) {
                cmr.idx = coll->getIndexCatalog()->findIndexByName(opCtx, indexName);
                if (!cmr.idx) {
                    return Status(ErrorCodes::IndexNotFound,
                                  str::stream()
                                      << "cannot find index " << indexName << " for ns " << nss);
                }
            } else {
                std::vector<const IndexDescriptor*> indexes;
                coll->getIndexCatalog()->findIndexesByKeyPattern(
                    opCtx, keyPattern, false, &indexes);

                if (indexes.size() > 1) {
                    return Status(ErrorCodes::AmbiguousIndexKeyPattern,
                                  str::stream() << "index keyPattern " << keyPattern << " matches "
                                                << indexes.size() << " indexes,"
                                                << " must use index name. "
                                                << "Conflicting indexes:" << indexes[0]->infoObj()
                                                << ", " << indexes[1]->infoObj());
                } else if (indexes.empty()) {
                    return Status(ErrorCodes::IndexNotFound,
                                  str::stream()
                                      << "cannot find index " << keyPattern << " for ns " << nss);
                }

                cmr.idx = indexes[0];
            }

            if (!cmr.indexExpireAfterSeconds.eoo()) {
                BSONElement oldExpireSecs = cmr.idx->infoObj().getField("expireAfterSeconds");
                if (oldExpireSecs.eoo()) {
                    return Status(ErrorCodes::InvalidOptions,
                                  "no expireAfterSeconds field to update");
                }
                if (!oldExpireSecs.isNumber()) {
                    return Status(ErrorCodes::InvalidOptions,
                                  "existing expireAfterSeconds field is not a number");
                }
            }
        } else if (fieldName == "validator" && !isView) {
            // Save this to a variable to avoid reading the atomic variable multiple times.
            const auto currentFCV = serverGlobalParams.featureCompatibility.getVersion();

            // If the feature compatibility version is not 4.6, and we are validating features as
            // master, ban the use of new agg features introduced in 4.6 to prevent them from being
            // persisted in the catalog.
            boost::optional<ServerGlobalParams::FeatureCompatibility::Version>
                maxFeatureCompatibilityVersion;
            if (serverGlobalParams.validateFeaturesAsMaster.load() &&
                currentFCV !=
                    ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo46) {
                maxFeatureCompatibilityVersion = currentFCV;
            }
            auto statusW = coll->parseValidator(opCtx,
                                                e.Obj(),
                                                MatchExpressionParser::kDefaultSpecialFeatures,
                                                maxFeatureCompatibilityVersion);
            if (!statusW.isOK()) {
                return statusW.getStatus();
            }

            cmr.collValidator = e;
        } else if (fieldName == "validationLevel" && !isView) {
            auto status = coll->parseValidationLevel(e.String());
            if (!status.isOK())
                return status;

            cmr.collValidationLevel = e.String();
        } else if (fieldName == "validationAction" && !isView) {
            auto status = coll->parseValidationAction(e.String());
            if (!status.isOK())
                return status;

            cmr.collValidationAction = e.String();
        } else if (fieldName == "pipeline") {
            if (!isView) {
                return Status(ErrorCodes::InvalidOptions,
                              "'pipeline' option only supported on a view");
            }
            if (e.type() != mongo::Array) {
                return Status(ErrorCodes::InvalidOptions, "not a valid aggregation pipeline");
            }
            cmr.viewPipeLine = e;
        } else if (fieldName == "viewOn") {
            if (!isView) {
                return Status(ErrorCodes::InvalidOptions,
                              "'viewOn' option only supported on a view");
            }
            if (e.type() != mongo::String) {
                return Status(ErrorCodes::InvalidOptions, "'viewOn' option must be a string");
            }
            cmr.viewOn = e.str();
        } else if (fieldName == "recordPreImages") {
            if (isView) {
                return {ErrorCodes::InvalidOptions,
                        str::stream() << "option not supported on a view: " << fieldName};
            }

            cmr.recordPreImages = e.trueValue();
        } else {
            if (isView) {
                return Status(ErrorCodes::InvalidOptions,
                              str::stream() << "option not supported on a view: " << fieldName);
            }

            return Status(ErrorCodes::InvalidOptions,
                          str::stream() << "unknown option to collMod: " << fieldName);
        }

        oplogEntryBuilder->append(e);
    }

    return {std::move(cmr)};
}

class CollModResultChange : public RecoveryUnit::Change {
public:
    CollModResultChange(const BSONElement& oldExpireSecs,
                        const BSONElement& newExpireSecs,
                        const BSONElement& oldHidden,
                        const BSONElement& newHidden,
                        BSONObjBuilder* result)
        : _oldExpireSecs(oldExpireSecs),
          _newExpireSecs(newExpireSecs),
          _oldHidden(oldHidden),
          _newHidden(newHidden),
          _result(result) {}

    void commit(boost::optional<Timestamp>) override {
        // add the fields to BSONObjBuilder result
        if (!_oldExpireSecs.eoo()) {
            _result->appendAs(_oldExpireSecs, "expireAfterSeconds_old");
            _result->appendAs(_newExpireSecs, "expireAfterSeconds_new");
        }
        if (!_newHidden.eoo()) {
            bool oldValue = _oldHidden.eoo() ? false : _oldHidden.booleanSafe();
            _result->append("hidden_old", oldValue);
            _result->appendAs(_newHidden, "hidden_new");
        }
    }

    void rollback() override {}

private:
    const BSONElement _oldExpireSecs;
    const BSONElement _newExpireSecs;
    const BSONElement _oldHidden;
    const BSONElement _newHidden;
    BSONObjBuilder* _result;
};

Status _collModInternal(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const BSONObj& cmdObj,
                        BSONObjBuilder* result) {
    StringData dbName = nss.db();
    AutoGetCollection autoColl(opCtx, nss, MODE_X, AutoGetCollection::ViewMode::kViewsPermitted);
    Lock::CollectionLock systemViewsLock(
        opCtx, NamespaceString(dbName, NamespaceString::kSystemDotViewsCollectionName), MODE_X);

    Database* const db = autoColl.getDb();
    Collection* coll = autoColl.getCollection();

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangAfterDatabaseLock, opCtx, "hangAfterDatabaseLock", []() {}, false, nss);

    // May also modify a view instead of a collection.
    boost::optional<ViewDefinition> view;
    if (db && !coll) {
        const auto sharedView = ViewCatalog::get(db)->lookup(opCtx, nss.ns());
        if (sharedView) {
            // We copy the ViewDefinition as it is modified below to represent the requested state.
            view = {*sharedView};
        }
    }

    // This can kill all cursors so don't allow running it while a background operation is in
    // progress.
    BackgroundOperation::assertNoBgOpInProgForNs(nss);
    if (coll) {
        IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(coll->uuid());
    }

    // If db/collection/view does not exist, short circuit and return.
    if (!db || (!coll && !view)) {
        return Status(ErrorCodes::NamespaceNotFound, "ns does not exist");
    }

    // This is necessary to set up CurOp, update the Top stats, and check shard version.
    OldClientContext ctx(opCtx, nss.ns());

    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::NotMaster,
                      str::stream() << "Not primary while setting collection options on " << nss);
    }

    BSONObjBuilder oplogEntryBuilder;
    auto statusW = parseCollModRequest(opCtx, nss, coll, cmdObj, &oplogEntryBuilder);
    if (!statusW.isOK()) {
        return statusW.getStatus();
    }
    auto oplogEntryObj = oplogEntryBuilder.obj();

    // Save both states of the CollModRequest to allow writeConflictRetries.
    const CollModRequest cmrOld = statusW.getValue();
    CollModRequest cmrNew = statusW.getValue();

    if (!cmrOld.indexHidden.eoo()) {

        if (serverGlobalParams.featureCompatibility.getVersion() <
                ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo46 &&
            cmrOld.indexHidden.booleanSafe()) {
            return Status(ErrorCodes::BadValue, "Hidden indexes can only be created with FCV 4.6");
        }
        if (coll->ns().isSystem())
            return Status(ErrorCodes::BadValue, "Can't hide index on system collection");
        if (cmrOld.idx->isIdIndex())
            return Status(ErrorCodes::BadValue, "can't hide _id index");
    }

    return writeConflictRetry(opCtx, "collMod", nss.ns(), [&] {
        WriteUnitOfWork wunit(opCtx);

        // Handle collMod on a view and return early. The View Catalog handles the creation of oplog
        // entries for modifications on a view.
        if (view) {
            if (!cmrOld.viewPipeLine.eoo())
                view->setPipeline(cmrOld.viewPipeLine);

            if (!cmrOld.viewOn.empty())
                view->setViewOn(NamespaceString(dbName, cmrOld.viewOn));

            ViewCatalog* catalog = ViewCatalog::get(db);

            BSONArrayBuilder pipeline;
            for (auto& item : view->pipeline()) {
                pipeline.append(item);
            }
            auto errorStatus =
                catalog->modifyView(opCtx, nss, view->viewOn(), BSONArray(pipeline.obj()));
            if (!errorStatus.isOK()) {
                return errorStatus;
            }

            wunit.commit();
            return Status::OK();
        }

        // In order to facilitate the replication rollback process, which makes a best effort
        // attempt to "undo" a set of oplog operations, we store a snapshot of the old collection
        // options to provide to the OpObserver. TTL index updates aren't a part of collection
        // options so we save the relevant TTL index data in a separate object.

        CollectionOptions oldCollOptions =
            DurableCatalog::get(opCtx)->getCollectionOptions(opCtx, coll->getCatalogId());

        boost::optional<IndexCollModInfo> indexCollModInfo;

        // Handle collMod operation type appropriately.

        if (!cmrOld.indexExpireAfterSeconds.eoo() || !cmrOld.indexHidden.eoo()) {
            BSONElement newExpireSecs = {};
            BSONElement oldExpireSecs = {};
            BSONElement newHidden = {};
            BSONElement oldHidden = {};
            // TTL Index
            if (!cmrOld.indexExpireAfterSeconds.eoo()) {
                newExpireSecs = cmrOld.indexExpireAfterSeconds;
                oldExpireSecs = cmrOld.idx->infoObj().getField("expireAfterSeconds");
                if (SimpleBSONElementComparator::kInstance.evaluate(oldExpireSecs !=
                                                                    newExpireSecs)) {
                    // Change the value of "expireAfterSeconds" on disk.
                    DurableCatalog::get(opCtx)->updateTTLSetting(opCtx,
                                                                 coll->getCatalogId(),
                                                                 cmrOld.idx->indexName(),
                                                                 newExpireSecs.safeNumberLong());
                }
            }

            // User wants to hide or unhide index.
            if (!cmrOld.indexHidden.eoo()) {
                newHidden = cmrOld.indexHidden;
                oldHidden = cmrOld.idx->infoObj().getField("hidden");
                // Make sure when we set 'hidden' to false, we can remove the hidden field from
                // catalog.
                if (SimpleBSONElementComparator::kInstance.evaluate(oldHidden != newHidden)) {
                    DurableCatalog::get(opCtx)->updateHiddenSetting(opCtx,
                                                                    coll->getCatalogId(),
                                                                    cmrOld.idx->indexName(),
                                                                    newHidden.booleanSafe());
                }
            }


            indexCollModInfo = IndexCollModInfo{
                cmrOld.indexExpireAfterSeconds.eoo() ? boost::optional<Seconds>()
                                                     : Seconds(newExpireSecs.safeNumberLong()),
                cmrOld.indexExpireAfterSeconds.eoo() ? boost::optional<Seconds>()
                                                     : Seconds(oldExpireSecs.safeNumberLong()),
                cmrOld.indexHidden.eoo() ? boost::optional<bool>() : newHidden.booleanSafe(),
                cmrOld.indexHidden.eoo() ? boost::optional<bool>() : oldHidden.booleanSafe(),
                cmrNew.idx->indexName()};

            // Notify the index catalog that the definition of this index changed. This will
            // invalidate the idx pointer in cmrOld. On rollback of this WUOW, the idx pointer
            // in cmrNew will be invalidated and the idx pointer in cmrOld will be valid again.
            cmrNew.idx = coll->getIndexCatalog()->refreshEntry(opCtx, cmrOld.idx);
            opCtx->recoveryUnit()->registerChange(std::make_unique<CollModResultChange>(
                oldExpireSecs, newExpireSecs, oldHidden, newHidden, result));

            if (MONGO_unlikely(assertAfterIndexUpdate.shouldFail())) {
                LOGV2(20307, "collMod - assertAfterIndexUpdate fail point enabled.");
                uasserted(50970, "trigger rollback after the index update");
            }
        }

        // The Validator, ValidationAction and ValidationLevel are already parsed and must be OK.
        if (!cmrNew.collValidator.eoo())
            invariant(coll->setValidator(opCtx, cmrNew.collValidator.Obj()));
        if (!cmrNew.collValidationAction.empty())
            invariant(coll->setValidationAction(opCtx, cmrNew.collValidationAction));
        if (!cmrNew.collValidationLevel.empty())
            invariant(coll->setValidationLevel(opCtx, cmrNew.collValidationLevel));

        if (cmrNew.recordPreImages != oldCollOptions.recordPreImages) {
            coll->setRecordPreImages(opCtx, cmrNew.recordPreImages);
        }

        // Only observe non-view collMods, as view operations are observed as operations on the
        // system.views collection.

        auto* const opObserver = opCtx->getServiceContext()->getOpObserver();
        opObserver->onCollMod(
            opCtx, nss, coll->uuid(), oplogEntryObj, oldCollOptions, indexCollModInfo);

        wunit.commit();
        return Status::OK();
    });
}

}  // namespace

Status collMod(OperationContext* opCtx,
               const NamespaceString& nss,
               const BSONObj& cmdObj,
               BSONObjBuilder* result) {
    return _collModInternal(opCtx, nss, cmdObj, result);
}

}  // namespace mongo
