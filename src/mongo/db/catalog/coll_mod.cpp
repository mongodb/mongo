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
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
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

void assertMovePrimaryInProgress(OperationContext* opCtx, NamespaceString const& nss) {
    Lock::DBLock dblock(opCtx, nss.db(), MODE_IS);
    auto dss = DatabaseShardingState::get(opCtx, nss.db().toString());
    if (!dss) {
        return;
    }

    auto dssLock = DatabaseShardingState::DSSLock::lockShared(opCtx, dss);
    try {
        const auto collDesc =
            CollectionShardingState::get(opCtx, nss)->getCollectionDescription();
        if (!collDesc.isSharded()) {
            auto mpsm = dss->getMovePrimarySourceManager(dssLock);

            if (mpsm) {
                LOGV2(
                    4945200, "assertMovePrimaryInProgress", "movePrimaryNss"_attr = nss.toString());

                uasserted(ErrorCodes::MovePrimaryInProgress,
                          "movePrimary is in progress for namespace " + nss.toString());
            }
        }
    } catch (const DBException& ex) {
        if (ex.toStatus() != ErrorCodes::MovePrimaryInProgress) {
            LOGV2(4945201, "Error when getting colleciton description", "what"_attr = ex.what());
            return;
        }
        throw;
    }
}

struct CollModRequest {
    const IndexDescriptor* idx = nullptr;
    BSONElement indexExpireAfterSeconds = {};
    BSONElement indexHidden = {};
    BSONElement viewPipeLine = {};
    std::string viewOn = {};
    boost::optional<Collection::Validator> collValidator;
    boost::optional<std::string> collValidationAction;
    boost::optional<std::string> collValidationLevel;
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

            // Hiding a hidden index or unhiding a visible index should be treated as a no-op.
            if (!cmr.indexHidden.eoo() && cmr.idx->hidden() == cmr.indexHidden.booleanSafe()) {
                // If the collMod includes "expireAfterSeconds", remove the no-op "hidden" parameter
                // and write the remaining "index" object to the oplog entry builder.
                if (!cmr.indexExpireAfterSeconds.eoo()) {
                    oplogEntryBuilder->append(fieldName, indexObj.removeField("hidden"));
                }
                // Un-set "indexHidden" in CollModRequest, and skip the automatic write to the
                // oplogEntryBuilder that occurs at the end of the parsing loop.
                cmr.indexHidden = {};
                continue;
            }
        } else if (fieldName == "validator" && !isView) {
            // Save this to a variable to avoid reading the atomic variable multiple times.
            const auto currentFCV = serverGlobalParams.featureCompatibility.getVersion();

            // If the feature compatibility version is not 4.4, and we are validating features as
            // master, ban the use of new agg features introduced in 4.4 to prevent them from being
            // persisted in the catalog.
            boost::optional<ServerGlobalParams::FeatureCompatibility::Version>
                maxFeatureCompatibilityVersion;
            if (serverGlobalParams.validateFeaturesAsMaster.load() &&
                currentFCV !=
                    ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44) {
                maxFeatureCompatibilityVersion = currentFCV;
            }
            cmr.collValidator = coll->parseValidator(opCtx,
                                                     e.Obj().getOwned(),
                                                     MatchExpressionParser::kDefaultSpecialFeatures,
                                                     maxFeatureCompatibilityVersion);
            if (!cmr.collValidator->isOK()) {
                return cmr.collValidator->getStatus();
            }
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
        assertMovePrimaryInProgress(opCtx, nss);
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
    CollModRequest cmrNew = std::move(statusW.getValue());
    auto viewPipeline = cmrNew.viewPipeLine;
    auto viewOn = cmrNew.viewOn;
    auto indexExpireAfterSeconds = cmrNew.indexExpireAfterSeconds;
    auto indexHidden = cmrNew.indexHidden;
    // WriteConflictExceptions thrown in the writeConflictRetry loop below can cause cmrNew.idx to
    // become invalid, so save a copy to use in the loop until we can refresh it.
    auto idx = cmrNew.idx;

    if (indexHidden) {
        if (serverGlobalParams.featureCompatibility.getVersion() <
                ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44 &&
            indexHidden.booleanSafe()) {
            return Status(ErrorCodes::BadValue, "Hidden indexes can only be created with FCV 4.4");
        }
        if (coll->ns().isSystem())
            return Status(ErrorCodes::BadValue, "Can't hide index on system collection");
        if (idx->isIdIndex())
            return Status(ErrorCodes::BadValue, "can't hide _id index");
    }

    return writeConflictRetry(opCtx, "collMod", nss.ns(), [&] {
        WriteUnitOfWork wunit(opCtx);

        // Handle collMod on a view and return early. The View Catalog handles the creation of oplog
        // entries for modifications on a view.
        if (view) {
            if (viewPipeline)
                view->setPipeline(viewPipeline);

            if (!viewOn.empty())
                view->setViewOn(NamespaceString(dbName, viewOn));

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

        if (indexExpireAfterSeconds || indexHidden) {
            BSONElement newExpireSecs = {};
            BSONElement oldExpireSecs = {};
            BSONElement newHidden = {};
            BSONElement oldHidden = {};

            // TTL Index
            if (indexExpireAfterSeconds) {
                newExpireSecs = indexExpireAfterSeconds;
                oldExpireSecs = idx->infoObj().getField("expireAfterSeconds");
                if (SimpleBSONElementComparator::kInstance.evaluate(oldExpireSecs !=
                                                                    newExpireSecs)) {
                    // Change the value of "expireAfterSeconds" on disk.
                    DurableCatalog::get(opCtx)->updateTTLSetting(opCtx,
                                                                 coll->getCatalogId(),
                                                                 idx->indexName(),
                                                                 newExpireSecs.safeNumberLong());
                }
            }

            // User wants to hide or unhide index.
            if (indexHidden) {
                newHidden = indexHidden;
                oldHidden = idx->infoObj().getField("hidden");
                // Make sure when we set 'hidden' to false, we can remove the hidden field from
                // catalog.
                if (SimpleBSONElementComparator::kInstance.evaluate(oldHidden != newHidden)) {
                    DurableCatalog::get(opCtx)->updateHiddenSetting(
                        opCtx, coll->getCatalogId(), idx->indexName(), newHidden.booleanSafe());
                }
            }

            indexCollModInfo =
                IndexCollModInfo{!indexExpireAfterSeconds ? boost::optional<Seconds>()
                                                          : Seconds(newExpireSecs.safeNumberLong()),
                                 !indexExpireAfterSeconds ? boost::optional<Seconds>()
                                                          : Seconds(oldExpireSecs.safeNumberLong()),
                                 !indexHidden ? boost::optional<bool>() : newHidden.booleanSafe(),
                                 !indexHidden ? boost::optional<bool>() : oldHidden.booleanSafe(),
                                 idx->indexName()};

            // Notify the index catalog that the definition of this index changed. This will
            // invalidate the local idx pointer. On rollback of this WUOW, the idx pointer in
            // cmrNew will be invalidated and the local var idx pointer will be valid again.
            cmrNew.idx = coll->getIndexCatalog()->refreshEntry(opCtx, idx);
            opCtx->recoveryUnit()->registerChange(std::make_unique<CollModResultChange>(
                oldExpireSecs, newExpireSecs, oldHidden, newHidden, result));

            if (MONGO_unlikely(assertAfterIndexUpdate.shouldFail())) {
                LOGV2(20307, "collMod - assertAfterIndexUpdate fail point enabled");
                uasserted(50970, "trigger rollback after the index update");
            }
        }

        if (cmrNew.collValidator) {
            coll->setValidator(opCtx, std::move(*cmrNew.collValidator));
        }
        if (cmrNew.collValidationAction)
            uassertStatusOKWithContext(
                coll->setValidationAction(opCtx, *cmrNew.collValidationAction),
                "Failed to set validationAction");
        if (cmrNew.collValidationLevel) {
            uassertStatusOKWithContext(coll->setValidationLevel(opCtx, *cmrNew.collValidationLevel),
                                       "Failed to set validationLevel");
        }

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
