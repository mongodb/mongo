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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/views/view_catalog.h"

#include <memory>
#include <string>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/audit.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/db/views/view.h"
#include "mongo/db/views/view_graph.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

namespace mongo {

namespace {
/**
 * Helper class to manage copy-on-write for the ViewCatalog.
 */
class ViewCatalogWriter {
public:
    ViewCatalogWriter(Mutex& mutex,
                      std::shared_ptr<const ViewCatalog> instance,
                      std::shared_ptr<ViewCatalog>* storage)
        : _mutex(mutex), _read(std::move(instance)), _storage(storage) {}

    ViewCatalogWriter(ViewCatalogWriter&&) = delete;
    ViewCatalogWriter& operator=(ViewCatalogWriter&&) = delete;

    const ViewCatalog* operator->() const {
        if (_write)
            return _write.get();

        return _read.get();
    }

    ViewCatalog* writable() {
        if (!_write) {
            _lock = stdx::unique_lock<Mutex>(_mutex);
            // TODO (SERVER-57250): This atomic_load will be deprecated in C++20
            // We must copy from `_storage` here under the lock so we include any changes that may
            // have happened since we copied '_read'.
            _write = std::make_shared<ViewCatalog>(*atomic_load(_storage));
            _read.reset();
        }
        return _write.get();
    }

    void commit() {
        if (_write) {
            atomic_store(_storage, _write);
            // Set _read and clear _write so we can use this instance in read mode after the commit.
            _read = std::move(_write);
            _lock.unlock();
        }
    }

private:
    Mutex& _mutex;
    stdx::unique_lock<Mutex> _lock;
    std::shared_ptr<const ViewCatalog> _read;
    std::shared_ptr<ViewCatalog> _write;
    std::shared_ptr<ViewCatalog>* _storage;
};

/**
 * Decoration on the ServiceContext for storing the latest ViewCatalog instance and its associated
 * write mutex.
 */
class ViewCatalogStorage {
public:
    std::shared_ptr<const ViewCatalog> get() const {
        return atomic_load(&_catalog);
    }

    void set(std::shared_ptr<ViewCatalog> instance) {
        atomic_store(&_catalog, std::move(instance));
    }

    ViewCatalogWriter writer() {
        return ViewCatalogWriter(_mutex, get(), &_catalog);
    }

    void setIgnoreExternalChange(StringData dbName, bool value) {
        stdx::lock_guard lk{_externalChangeMutex};
        if (value) {
            _ignoreExternalChange.emplace(dbName);
        } else {
            _ignoreExternalChange.erase(dbName);
        }
    }

    bool shouldIgnoreExternalChange(StringData dbName) const {
        stdx::lock_guard lk{_externalChangeMutex};
        auto it = _ignoreExternalChange.find(dbName);
        return it != _ignoreExternalChange.end();
    }

private:
    std::shared_ptr<ViewCatalog> _catalog = std::make_shared<ViewCatalog>();
    mutable Mutex _mutex = MONGO_MAKE_LATCH("ViewCatalogStorage::_mutex");  // Serializes writes
    mutable Mutex _externalChangeMutex = MONGO_MAKE_LATCH(
        "ViewCatalogStorage::_externalChangeMutex");  // Guards _ignoreExternalChange set
    StringSet _ignoreExternalChange;
};  // namespace
const auto getViewCatalog = ServiceContext::declareDecoration<ViewCatalogStorage>();

StatusWith<std::unique_ptr<CollatorInterface>> parseCollator(OperationContext* opCtx,
                                                             BSONObj collationSpec) {
    // If 'collationSpec' is empty, return the null collator, which represents the "simple"
    // collation.
    if (collationSpec.isEmpty()) {
        return {nullptr};
    }
    return CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collationSpec);
}
}  // namespace

std::shared_ptr<const ViewCatalog> ViewCatalog::get(ServiceContext* svcCtx) {
    return getViewCatalog(svcCtx).get();
}

std::shared_ptr<const ViewCatalog> ViewCatalog::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

Status ViewCatalog::registerDatabase(OperationContext* opCtx,
                                     StringData dbName,
                                     std::unique_ptr<DurableViewCatalog> durable) {
    auto catalog = getViewCatalog(opCtx->getServiceContext()).writer();
    auto it = catalog.writable()->_viewsForDatabase.find(dbName);
    if (it != catalog.writable()->_viewsForDatabase.end()) {
        return {ErrorCodes::AlreadyInitialized, "ViewCatalog entry for database already set"};
    }

    auto& vfdb = catalog.writable()->_viewsForDatabase[dbName];
    vfdb.durable = std::move(durable);
    vfdb.valid = false;
    vfdb.viewGraphNeedsRefresh = true;
    catalog.commit();
    return Status::OK();
}

void ViewCatalog::unregisterDatabase(OperationContext* opCtx, Database* db) {
    auto catalog = getViewCatalog(opCtx->getServiceContext()).writer();
    auto it = catalog.writable()->_viewsForDatabase.find(db->name().dbName());
    if (it != catalog.writable()->_viewsForDatabase.end() && it->second.durable->belongsTo(db)) {
        catalog.writable()->_viewsForDatabase.erase(it);
        catalog.commit();
    }
}

Status ViewCatalog::reload(OperationContext* opCtx,
                           StringData dbName,
                           ViewCatalogLookupBehavior lookupBehavior) {
    auto catalog = getViewCatalog(opCtx->getServiceContext()).writer();
    invariant(opCtx->lockState()->isCollectionLockedForMode(
        NamespaceString(dbName, NamespaceString::kSystemDotViewsCollectionName), MODE_IS));
    auto result = catalog.writable()->_reload(
        opCtx, dbName, ViewCatalogLookupBehavior::kValidateDurableViews, true);
    catalog.commit();
    return result;
}

Status ViewCatalog::_reload(OperationContext* opCtx,
                            StringData dbName,
                            ViewCatalogLookupBehavior lookupBehavior,
                            bool reloadForCollectionCatalog) {
    LOGV2_DEBUG(22546, 1, "Reloading view catalog for database", "db"_attr = dbName);

    auto it = _viewsForDatabase.find(dbName);
    invariant(it != _viewsForDatabase.end());
    auto& vfdb = it->second;

    vfdb.viewMap.clear();
    vfdb.valid = false;
    vfdb.viewGraphNeedsRefresh = true;
    vfdb.stats = {};

    absl::flat_hash_set<NamespaceString> viewNamesForDb;

    auto reloadCallback = [&](const BSONObj& view) -> Status {
        BSONObj collationSpec = view.hasField("collation") ? view["collation"].Obj() : BSONObj();
        auto collator = parseCollator(opCtx, collationSpec);
        if (!collator.isOK()) {
            return collator.getStatus();
        }

        NamespaceString viewName(view["_id"].str());

        auto pipeline = view["pipeline"].Obj();
        for (auto&& stage : pipeline) {
            if (BSONType::Object != stage.type()) {
                return Status(ErrorCodes::InvalidViewDefinition,
                              str::stream() << "View 'pipeline' entries must be objects, but "
                                            << viewName.toString()
                                            << " has a pipeline element of type " << stage.type());
            }
        }

        auto viewDef = std::make_shared<ViewDefinition>(viewName.db(),
                                                        viewName.coll(),
                                                        view["viewOn"].str(),
                                                        pipeline,
                                                        std::move(collator.getValue()));

        if (!viewName.isOnInternalDb() && !viewName.isSystem()) {
            if (viewDef->timeseries()) {
                vfdb.stats.userTimeseries += 1;
            } else {
                vfdb.stats.userViews += 1;
            }
        } else {
            vfdb.stats.internal += 1;
        }

        vfdb.viewMap[viewName.ns()] = std::move(viewDef);
        if (reloadForCollectionCatalog) {
            viewNamesForDb.insert(viewName);
        }
        return Status::OK();
    };

    try {
        if (lookupBehavior == ViewCatalogLookupBehavior::kValidateDurableViews) {
            vfdb.durable->iterate(opCtx, reloadCallback);
        } else if (lookupBehavior == ViewCatalogLookupBehavior::kAllowInvalidDurableViews) {
            vfdb.durable->iterateIgnoreInvalidEntries(opCtx, reloadCallback);
        } else {
            MONGO_UNREACHABLE;
        }
        if (reloadForCollectionCatalog) {
            CollectionCatalog::write(
                opCtx,
                [&dbName, viewsForDb = std::move(viewNamesForDb)](CollectionCatalog& catalog) {
                    catalog.replaceViewsForDatabase(dbName, std::move(viewsForDb));
                });
        }
    } catch (const DBException& ex) {
        auto status = ex.toStatus();
        LOGV2(22547,
              "Could not load view catalog for database",
              "db"_attr = vfdb.durable->getName(),
              "error"_attr = status);
        return status;
    }

    vfdb.valid = true;
    return Status::OK();
}

void ViewCatalog::clear(OperationContext* opCtx, StringData dbName) {
    auto catalog = getViewCatalog(opCtx->getServiceContext()).writer();
    auto it = catalog.writable()->_viewsForDatabase.find(dbName);
    invariant(it != catalog.writable()->_viewsForDatabase.end());
    auto& vfdb = it->second;

    // First, iterate through the views on this database and audit them before they are dropped.
    for (auto&& view : vfdb.viewMap) {
        audit::logDropView(opCtx->getClient(),
                           (*view.second).name(),
                           (*view.second).viewOn().ns(),
                           (*view.second).pipeline(),
                           ErrorCodes::OK);
    }

    vfdb.viewMap.clear();
    vfdb.viewGraph.clear();
    vfdb.valid = true;
    vfdb.viewGraphNeedsRefresh = false;
    vfdb.stats = {};
    CollectionCatalog::write(opCtx, [db = dbName.toString()](CollectionCatalog& catalog) {
        catalog.replaceViewsForDatabase(db, {});
    });
    catalog.commit();
}

bool ViewCatalog::shouldIgnoreExternalChange(OperationContext* opCtx, const NamespaceString& name) {
    return getViewCatalog(opCtx->getServiceContext()).shouldIgnoreExternalChange(name.db());
}

void ViewCatalog::ViewsForDatabase::requireValidCatalog() const {
    uassert(ErrorCodes::InvalidViewDefinition,
            "Invalid view definition detected in the view catalog. Remove the invalid view "
            "manually to prevent disallowing any further usage of the view catalog.",
            valid);
}

void ViewCatalog::iterate(StringData dbName, ViewIteratorCallback callback) const {
    auto it = _viewsForDatabase.find(dbName);
    if (it == _viewsForDatabase.end()) {
        return;
    }
    auto& vfdb = it->second;

    vfdb.requireValidCatalog();
    for (auto&& view : vfdb.viewMap) {
        if (!callback(*view.second)) {
            break;
        }
    }
}

Status ViewCatalog::_createOrUpdateView(OperationContext* opCtx,
                                        const NamespaceString& viewName,
                                        const NamespaceString& viewOn,
                                        const BSONArray& pipeline,
                                        std::unique_ptr<CollatorInterface> collator) {
    invariant(opCtx->lockState()->isDbLockedForMode(viewName.db(), MODE_IX));
    invariant(opCtx->lockState()->isCollectionLockedForMode(viewName, MODE_IX));
    invariant(opCtx->lockState()->isCollectionLockedForMode(
        NamespaceString(viewName.db(), NamespaceString::kSystemDotViewsCollectionName), MODE_X));

    auto it = _viewsForDatabase.find(viewName.db());
    invariant(it != _viewsForDatabase.end());
    auto& vfdb = it->second;
    vfdb.requireValidCatalog();

    // Build the BSON definition for this view to be saved in the durable view catalog. If the
    // collation is empty, omit it from the definition altogether.
    BSONObjBuilder viewDefBuilder;
    viewDefBuilder.append("_id", viewName.ns());
    viewDefBuilder.append("viewOn", viewOn.coll());
    viewDefBuilder.append("pipeline", pipeline);
    if (collator) {
        viewDefBuilder.append("collation", collator->getSpec().toBSON());
    }

    BSONObj ownedPipeline = pipeline.getOwned();
    auto view = std::make_shared<ViewDefinition>(
        viewName.db(), viewName.coll(), viewOn.coll(), ownedPipeline, std::move(collator));

    // Check that the resulting dependency graph is acyclic and within the maximum depth.
    Status graphStatus = _upsertIntoGraph(opCtx, *(view.get()));
    if (!graphStatus.isOK()) {
        return graphStatus;
    }

    vfdb.durable->upsert(opCtx, viewName, viewDefBuilder.obj());
    vfdb.viewMap[viewName.ns()] = view;

    // Reload the view catalog with the changes applied.
    auto res =
        _reload(opCtx, viewName.db(), ViewCatalogLookupBehavior::kValidateDurableViews, false);
    if (res.isOK()) {
        // Register the view in the CollectionCatalog mapping from ResourceID->namespace
        auto viewRid = ResourceId(RESOURCE_COLLECTION, viewName.ns());

        CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
            catalog.registerView(viewName);
            catalog.addResource(viewRid, viewName.ns());
        });

        opCtx->recoveryUnit()->onRollback([viewName, opCtx, viewRid]() {
            CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
                catalog.removeResource(viewRid, viewName.ns());
                catalog.deregisterView(viewName);
            });
        });
    }
    return res;
}

Status ViewCatalog::_upsertIntoGraph(OperationContext* opCtx, const ViewDefinition& viewDef) {
    auto it = _viewsForDatabase.find(viewDef.name().db());
    invariant(it != _viewsForDatabase.end());
    auto& vfdb = it->second;

    // Performs the insert into the graph.
    auto doInsert = [this, opCtx, &vfdb](const ViewDefinition& viewDef,
                                         bool needsValidation) -> Status {
        // Validate that the pipeline is eligible to serve as a view definition. If it is, this
        // will also return the set of involved namespaces.
        auto pipelineStatus = validatePipeline(opCtx, viewDef);
        if (!pipelineStatus.isOK()) {
            if (needsValidation) {
                uassertStatusOKWithContext(pipelineStatus.getStatus(),
                                           str::stream() << "Invalid pipeline for view "
                                                         << viewDef.name().ns());
            }
            return pipelineStatus.getStatus();
        }

        auto involvedNamespaces = pipelineStatus.getValue();
        std::vector<NamespaceString> refs(involvedNamespaces.begin(), involvedNamespaces.end());
        refs.push_back(viewDef.viewOn());

        int pipelineSize = 0;
        for (auto obj : viewDef.pipeline()) {
            pipelineSize += obj.objsize();
        }

        if (needsValidation) {
            // Check the collation of all the dependent namespaces before updating the graph.
            auto collationStatus = _validateCollation(opCtx, viewDef, refs);
            if (!collationStatus.isOK()) {
                return collationStatus;
            }
            return vfdb.viewGraph.insertAndValidate(viewDef, refs, pipelineSize);
        } else {
            vfdb.viewGraph.insertWithoutValidating(viewDef, refs, pipelineSize);
            return Status::OK();
        }
    };

    if (vfdb.viewGraphNeedsRefresh) {
        vfdb.viewGraph.clear();
        for (auto&& iter : vfdb.viewMap) {
            auto status = doInsert(*(iter.second.get()), false);
            // If we cannot fully refresh the graph, we will keep '_viewGraphNeedsRefresh' true.
            if (!status.isOK()) {
                return status;
            }
        }
        // Only if the inserts completed without error will we no longer need a refresh.
        vfdb.viewGraphNeedsRefresh = false;
    }

    // Remove the view definition first in case this is an update. If it is not in the graph, it
    // is simply a no-op.
    vfdb.viewGraph.remove(viewDef.name());

    return doInsert(viewDef, true);
}

StatusWith<stdx::unordered_set<NamespaceString>> ViewCatalog::validatePipeline(
    OperationContext* opCtx, const ViewDefinition& viewDef) {
    const LiteParsedPipeline liteParsedPipeline(viewDef.viewOn(), viewDef.pipeline());
    const auto involvedNamespaces = liteParsedPipeline.getInvolvedNamespaces();

    // The API version pipeline validation should be skipped for time-series view because of
    // following reasons:
    //     - the view pipeline is not created by (or visible to) the end-user and should be skipped.
    //     - the view pipeline can have stages that are not allowed in stable API version '1' eg.
    //       '$_internalUnpackBucket'.
    bool performApiVersionChecks = !viewDef.timeseries();

    liteParsedPipeline.validate(opCtx, performApiVersionChecks);

    // Verify that this is a legitimate pipeline specification by making sure it parses
    // correctly. In order to parse a pipeline we need to resolve any namespaces involved to a
    // collection and a pipeline, but in this case we don't need this map to be accurate since
    // we will not be evaluating the pipeline.
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    for (auto&& nss : involvedNamespaces) {
        resolvedNamespaces[nss.coll()] = {nss, {}};
    }
    boost::intrusive_ptr<ExpressionContext> expCtx =
        new ExpressionContext(opCtx,
                              AggregateCommandRequest(viewDef.viewOn(), viewDef.pipeline()),
                              CollatorInterface::cloneCollator(viewDef.defaultCollator()),
                              // We can use a stub MongoProcessInterface because we are only parsing
                              // the Pipeline for validation here. We won't do anything with the
                              // pipeline that will require a real implementation.
                              std::make_shared<StubMongoProcessInterface>(),
                              std::move(resolvedNamespaces),
                              boost::none);

    // If the feature compatibility version is not kLatest, and we are validating features as
    // primary, ban the use of new agg features introduced in kLatest to prevent them from being
    // persisted in the catalog.
    // (Generic FCV reference): This FCV check should exist across LTS binary versions.
    multiversion::FeatureCompatibilityVersion fcv;
    if (serverGlobalParams.validateFeaturesAsPrimary.load() &&
        serverGlobalParams.featureCompatibility.isLessThan(multiversion::GenericFCV::kLatest,
                                                           &fcv)) {
        expCtx->maxFeatureCompatibilityVersion = fcv;
    }

    // The pipeline parser needs to know that we're parsing a pipeline for a view definition
    // to apply some additional checks.
    expCtx->isParsingViewDefinition = true;

    try {
        auto pipeline =
            Pipeline::parse(viewDef.pipeline(), std::move(expCtx), [&](const Pipeline& pipeline) {
                // Validate that the view pipeline does not contain any ineligible stages.
                const auto& sources = pipeline.getSources();
                const auto firstPersistentStage =
                    std::find_if(sources.begin(), sources.end(), [](const auto& source) {
                        return source->constraints().writesPersistentData();
                    });

                uassert(ErrorCodes::OptionNotSupportedOnView,
                        str::stream()
                            << "The aggregation stage "
                            << firstPersistentStage->get()->getSourceName() << " in location "
                            << std::distance(sources.begin(), firstPersistentStage)
                            << " of the pipeline cannot be used in the view definition of "
                            << viewDef.name().ns() << " because it writes to disk",
                        firstPersistentStage == sources.end());

                uassert(ErrorCodes::OptionNotSupportedOnView,
                        "$changeStream cannot be used in a view definition",
                        sources.empty() || !sources.front()->constraints().isChangeStreamStage());

                std::for_each(sources.begin(), sources.end(), [](auto& stage) {
                    uassert(ErrorCodes::InvalidNamespace,
                            str::stream() << "'" << stage->getSourceName()
                                          << "' cannot be used in a view definition",
                            !stage->constraints().isIndependentOfAnyCollection);
                });
            });
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    return std::move(involvedNamespaces);
}

Status ViewCatalog::_validateCollation(OperationContext* opCtx,
                                       const ViewDefinition& view,
                                       const std::vector<NamespaceString>& refs) const {
    for (auto&& potentialViewNss : refs) {
        auto otherView =
            _lookup(opCtx, potentialViewNss, ViewCatalogLookupBehavior::kValidateDurableViews);
        if (otherView &&
            !CollatorInterface::collatorsMatch(view.defaultCollator(),
                                               otherView->defaultCollator())) {
            return {ErrorCodes::OptionNotSupportedOnView,
                    str::stream() << "View " << view.name().toString()
                                  << " has conflicting collation with view "
                                  << otherView->name().toString()};
        }
    }
    return Status::OK();
}

Status ViewCatalog::createView(OperationContext* opCtx,
                               const NamespaceString& viewName,
                               const NamespaceString& viewOn,
                               const BSONArray& pipeline,
                               const BSONObj& collation) {
    invariant(opCtx->lockState()->isDbLockedForMode(viewName.db(), MODE_IX));
    invariant(opCtx->lockState()->isCollectionLockedForMode(viewName, MODE_IX));
    invariant(opCtx->lockState()->isCollectionLockedForMode(
        NamespaceString(viewName.db(), NamespaceString::kSystemDotViewsCollectionName), MODE_X));

    auto& catalogStorage = getViewCatalog(opCtx->getServiceContext());
    auto catalog = catalogStorage.writer();

    if (viewName.db() != viewOn.db())
        return Status(ErrorCodes::BadValue,
                      "View must be created on a view or collection in the same database");

    if (catalog->_lookup(opCtx, viewName, ViewCatalogLookupBehavior::kValidateDurableViews))
        return Status(ErrorCodes::NamespaceExists, "Namespace already exists");

    if (!NamespaceString::validCollectionName(viewOn.coll()))
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid name for 'viewOn': " << viewOn.coll());

    auto collator = parseCollator(opCtx, collation);
    if (!collator.isOK())
        return collator.getStatus();

    Status result = Status::OK();
    {
        ON_BLOCK_EXIT([&catalogStorage, &viewName] {
            catalogStorage.setIgnoreExternalChange(viewName.db(), false);
        });
        catalogStorage.setIgnoreExternalChange(viewName.db(), true);

        result = catalog.writable()->_createOrUpdateView(
            opCtx, viewName, viewOn, pipeline, std::move(collator.getValue()));
    }
    if (result.isOK()) {
        catalog.commit();
    }
    return result;
}

Status ViewCatalog::modifyView(OperationContext* opCtx,
                               const NamespaceString& viewName,
                               const NamespaceString& viewOn,
                               const BSONArray& pipeline) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(viewName, MODE_X));
    invariant(opCtx->lockState()->isCollectionLockedForMode(
        NamespaceString(viewName.db(), NamespaceString::kSystemDotViewsCollectionName), MODE_X));

    auto& catalogStorage = getViewCatalog(opCtx->getServiceContext());
    auto catalog = catalogStorage.writer();

    if (viewName.db() != viewOn.db())
        return Status(ErrorCodes::BadValue,
                      "View must be created on a view or collection in the same database");

    auto viewPtr =
        catalog->_lookup(opCtx, viewName, ViewCatalogLookupBehavior::kValidateDurableViews);
    if (!viewPtr)
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "cannot modify missing view " << viewName.ns());

    if (!NamespaceString::validCollectionName(viewOn.coll()))
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid name for 'viewOn': " << viewOn.coll());

    opCtx->recoveryUnit()->onRollback([viewName, opCtx]() {
        auto viewRid = ResourceId(RESOURCE_COLLECTION, viewName.ns());

        CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
            catalog.addResource(viewRid, viewName.ns());
        });
    });

    Status result = Status::OK();
    {
        ON_BLOCK_EXIT([&catalogStorage, &viewName] {
            catalogStorage.setIgnoreExternalChange(viewName.db(), false);
        });
        catalogStorage.setIgnoreExternalChange(viewName.db(), true);

        result = catalog.writable()->_createOrUpdateView(
            opCtx,
            viewName,
            viewOn,
            pipeline,
            CollatorInterface::cloneCollator(viewPtr->defaultCollator()));
    }

    if (result.isOK()) {
        catalog.commit();
    }

    return result;
}

Status ViewCatalog::dropView(OperationContext* opCtx, const NamespaceString& viewName) {
    invariant(opCtx->lockState()->isDbLockedForMode(viewName.db(), MODE_IX));
    invariant(opCtx->lockState()->isCollectionLockedForMode(viewName, MODE_IX));
    invariant(opCtx->lockState()->isCollectionLockedForMode(
        NamespaceString(viewName.db(), NamespaceString::kSystemDotViewsCollectionName), MODE_X));

    auto& catalogStorage = getViewCatalog(opCtx->getServiceContext());
    auto catalog = catalogStorage.writer();

    auto it = catalog.writable()->_viewsForDatabase.find(viewName.db());
    invariant(it != catalog.writable()->_viewsForDatabase.end());
    auto& vfdb = it->second;
    vfdb.requireValidCatalog();

    Status result = Status::OK();

    {
        ON_BLOCK_EXIT([&catalogStorage, &viewName] {
            catalogStorage.setIgnoreExternalChange(viewName.db(), false);
        });

        catalogStorage.setIgnoreExternalChange(viewName.db(), true);

        // Save a copy of the view definition in case we need to roll back.
        auto viewPtr =
            catalog->_lookup(opCtx, viewName, ViewCatalogLookupBehavior::kValidateDurableViews);
        if (!viewPtr) {
            return {ErrorCodes::NamespaceNotFound,
                    str::stream() << "cannot drop missing view: " << viewName.ns()};
        }

        invariant(vfdb.valid);
        vfdb.durable->remove(opCtx, viewName);
        vfdb.viewGraph.remove(viewPtr->name());
        vfdb.viewMap.erase(viewName.ns());

        auto viewRid = ResourceId(RESOURCE_COLLECTION, viewName.ns());
        CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
            catalog.removeResource(viewRid, viewName.ns());
        });

        opCtx->recoveryUnit()->onCommit([viewName, opCtx](auto ts) {
            CollectionCatalog::write(
                opCtx, [&](CollectionCatalog& catalog) { catalog.deregisterView(viewName); });
        });

        opCtx->recoveryUnit()->onRollback([viewName, opCtx, viewRid]() {
            CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
                catalog.addResource(viewRid, viewName.ns());
            });
        });

        // Reload the view catalog with the changes applied.
        result = catalog.writable()->_reload(
            opCtx, viewName.db(), ViewCatalogLookupBehavior::kValidateDurableViews, false);
    }
    catalog.commit();
    return result;
}

std::shared_ptr<const ViewDefinition> ViewCatalog::_lookup(
    OperationContext* opCtx,
    const NamespaceString& ns,
    ViewCatalogLookupBehavior lookupBehavior) const {
    auto it = _viewsForDatabase.find(ns.db());
    if (it == _viewsForDatabase.end()) {
        return nullptr;
    }
    auto& vfdb = it->second;

    ViewMap::const_iterator vmit = vfdb.viewMap.find(ns.ns());
    if (vmit != vfdb.viewMap.end()) {
        return vmit->second;
    }
    return nullptr;
}

std::shared_ptr<ViewDefinition> ViewCatalog::_lookup(OperationContext* opCtx,
                                                     const NamespaceString& ns,
                                                     ViewCatalogLookupBehavior lookupBehavior) {
    return std::const_pointer_cast<ViewDefinition>(
        std::as_const(*this)._lookup(opCtx, ns, lookupBehavior));
}

std::shared_ptr<const ViewDefinition> ViewCatalog::lookup(OperationContext* opCtx,
                                                          const NamespaceString& ns) const {
    auto it = _viewsForDatabase.find(ns.db());
    if (it == _viewsForDatabase.end()) {
        return nullptr;
    }
    auto& vfdb = it->second;

    if (!vfdb.valid && opCtx->getClient()->isFromUserConnection()) {
        // We want to avoid lookups on invalid collection names.
        if (!NamespaceString::validCollectionName(ns.ns())) {
            return nullptr;
        }

        // ApplyOps should work on a valid existing collection, despite the presence of bad views
        // otherwise the server would crash. The view catalog will remain invalid until the bad view
        // definitions are removed.
        vfdb.requireValidCatalog();
    }

    return _lookup(opCtx, ns, ViewCatalogLookupBehavior::kValidateDurableViews);
}

std::shared_ptr<const ViewDefinition> ViewCatalog::lookupWithoutValidatingDurableViews(
    OperationContext* opCtx, const NamespaceString& ns) const {
    return _lookup(opCtx, ns, ViewCatalogLookupBehavior::kAllowInvalidDurableViews);
}

StatusWith<ResolvedView> ViewCatalog::resolveView(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<BSONObj> timeSeriesCollator) const {
    auto it = _viewsForDatabase.find(nss.db());
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "View " << nss << " not found",
            it != _viewsForDatabase.end());
    auto& vfdb = it->second;
    vfdb.requireValidCatalog();

    // Points to the name of the most resolved namespace.
    const NamespaceString* resolvedNss = &nss;

    // Holds the combination of all the resolved views.
    std::vector<BSONObj> resolvedPipeline;

    // If the catalog has not been tampered with, all views seen during the resolution will have
    // the same collation. As an optimization, we fill out the collation spec only once.
    boost::optional<BSONObj> collation;

    // The last seen view definition, which owns the NamespaceString pointed to by
    // 'resolvedNss'.
    std::shared_ptr<ViewDefinition> lastViewDefinition;

    std::vector<NamespaceString> dependencyChain{nss};

    int depth = 0;
    boost::optional<bool> mixedData = boost::none;
    boost::optional<TimeseriesOptions> tsOptions = boost::none;
    for (; depth < ViewGraph::kMaxViewDepth; depth++) {
        auto view = _lookup(opCtx, *resolvedNss, ViewCatalogLookupBehavior::kValidateDurableViews);
        if (!view) {
            // Return error status if pipeline is too large.
            int pipelineSize = 0;
            for (auto obj : resolvedPipeline) {
                pipelineSize += obj.objsize();
            }
            if (pipelineSize > ViewGraph::kMaxViewPipelineSizeBytes) {
                return {ErrorCodes::ViewPipelineMaxSizeExceeded,
                        str::stream() << "View pipeline exceeds maximum size; maximum size is "
                                      << ViewGraph::kMaxViewPipelineSizeBytes};
            }

            auto curOp = CurOp::get(opCtx);
            curOp->debug().addResolvedViews(dependencyChain, resolvedPipeline);

            return StatusWith<ResolvedView>(
                {*resolvedNss,
                 std::move(resolvedPipeline),
                 collation ? std::move(collation.get()) : CollationSpec::kSimpleSpec,
                 tsOptions,
                 mixedData});
        }

        resolvedNss = &view->viewOn();

        if (view->timeseries()) {
            // Use the lock-free collection lookup, to ensure compatibility with lock-free read
            // operations.
            auto tsCollection = CollectionCatalog::get(opCtx)->lookupCollectionByNamespaceForRead(
                opCtx, *resolvedNss);
            uassert(6067201,
                    str::stream() << "expected time-series buckets collection " << *resolvedNss
                                  << " to exist",
                    tsCollection);
            if (tsCollection) {
                mixedData = tsCollection->getTimeseriesBucketsMayHaveMixedSchemaData();
                tsOptions = tsCollection->getTimeseriesOptions();
            }
        }

        dependencyChain.push_back(*resolvedNss);
        if (!collation) {
            if (timeSeriesCollator) {
                collation = *timeSeriesCollator;
            } else {
                collation = view->defaultCollator() ? view->defaultCollator()->getSpec().toBSON()
                                                    : CollationSpec::kSimpleSpec;
            }
        }

        // Prepend the underlying view's pipeline to the current working pipeline.
        const std::vector<BSONObj>& toPrepend = view->pipeline();
        resolvedPipeline.insert(resolvedPipeline.begin(), toPrepend.begin(), toPrepend.end());

        // If the first stage is a $collStats, then we return early with the viewOn namespace.
        if (toPrepend.size() > 0 && !toPrepend[0]["$collStats"].eoo()) {
            auto curOp = CurOp::get(opCtx);
            curOp->debug().addResolvedViews(dependencyChain, resolvedPipeline);

            return StatusWith<ResolvedView>(
                {*resolvedNss, std::move(resolvedPipeline), std::move(collation.get())});
        }
    }

    if (depth >= ViewGraph::kMaxViewDepth) {
        return {ErrorCodes::ViewDepthLimitExceeded,
                str::stream() << "View depth too deep or view cycle detected; maximum depth is "
                              << ViewGraph::kMaxViewDepth};
    }

    MONGO_UNREACHABLE;
}

boost::optional<ViewCatalog::Stats> ViewCatalog::getStats(StringData dbName) const {
    auto it = _viewsForDatabase.find(dbName);
    if (it == _viewsForDatabase.end()) {
        return boost::none;
    }
    auto& vfdb = it->second;
    return vfdb.stats;
}
}  // namespace mongo
