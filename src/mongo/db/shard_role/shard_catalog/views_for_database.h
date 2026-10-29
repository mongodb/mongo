// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/views/view.h"
#include "mongo/db/views/view_graph.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <functional>
#include <memory>
#include <vector>

namespace mongo {

/**
 * Holds all data for the views associated with a particular database.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ViewsForDatabase {
public:
    using PipelineValidatorFn = std::function<StatusWith<stdx::unordered_set<NamespaceString>>(
        OperationContext*, const ViewDefinition&)>;

    /**
     * Usage statistics about the views associated with a single database.
     * Total views = internal + userViews + userTimeseries.
     */
    struct Stats {
        int userViews = 0;
        // TODO(SERVER-106490): Remove once 9.0 becomes last LTS and timeseries views are phased out
        int userTimeseries = 0;
        int internal = 0;
    };

    enum class Durability {
        // The view is not yet inserted into the system.views collection.
        kNotYetDurable,

        // The view is already present in the system.views collection.
        kAlreadyDurable,
    };

    bool allViewsAreValid() const {
        return _invalidViewNames.empty() && !_hasInvalidNss;
    }

    bool isViewValid(const NamespaceString& nss) const {
        return !_invalidViewNames.contains(nss);
    }

    Stats stats() const {
        return _stats;
    }

    std::shared_ptr<const ViewDefinition> lookup(const NamespaceString& ns) const;

    void iterate(const std::function<bool(const ViewDefinition& view)>& callback) const;

    /**
     * Reloads all the valid views from the system.views collection.
     * Invalid views are noted internally
     */
    void reloadValidViews(OperationContext* opCtx, const CollectionPtr& systemViews);

    /**
     * Reloads views from the system.views collection and reports error if at least one view is
     * invalid
     */
    Status reloadAllViews(OperationContext* opCtx, const CollectionPtr& systemViews);

    Status insert(OperationContext* opCtx,
                  const CollectionPtr& systemViews,
                  const NamespaceString& viewName,
                  const NamespaceString& viewOn,
                  const BSONArray& pipeline,
                  const PipelineValidatorFn& validatePipeline,
                  const BSONObj& collator,
                  Durability durability);

    Status update(OperationContext* opCtx,
                  const CollectionPtr& systemViews,
                  const NamespaceString& viewName,
                  const NamespaceString& viewOn,
                  const BSONArray& pipeline,
                  const PipelineValidatorFn& validatePipeline,
                  std::unique_ptr<CollatorInterface> collator);

    void remove(OperationContext* opCtx,
                const CollectionPtr& systemViews,
                const NamespaceString& ns);

    void clear(OperationContext* opCtx);

private:
    Status _reload(OperationContext* opCtx, const CollectionPtr& systemViews, bool errorOnInvalid);
    /**
     * Inserts or updates the given view into the view map.
     */
    void _upsertIntoMap(OperationContext* opCtx, std::shared_ptr<ViewDefinition> view);

    /**
     * Parses the view definition pipeline, attempts to upsert into the view graph, and refreshes
     * the graph if necessary. Returns an error status if the resulting graph would be invalid.
     * needsValidation controls whether we check that the resulting dependency graph is acyclic and
     * within the maximum depth.
     */
    Status _upsertIntoGraph(OperationContext* opCtx,
                            const ViewDefinition& viewDef,
                            const PipelineValidatorFn& validatePipeline,
                            bool needsValidation);

    /**
     * Inserts or updates the given view into the system.views collection.
     */
    Status _upsertIntoCatalog(OperationContext* opCtx,
                              const CollectionPtr& systemViews,
                              const ViewDefinition& view);

    /**
     * Returns OK if each view namespace in 'refs' has the same default collation as the given view.
     * Otherwise, returns ErrorCodes::OptionNotSupportedOnView.
     */
    Status _validateCollation(OperationContext* opCtx,
                              const ViewDefinition& view,
                              const std::vector<NamespaceString>& refs) const;

    StringMap<std::shared_ptr<ViewDefinition>> _viewMap;
    ViewGraph _viewGraph;

    // Store the list of views nss that are corrupted. At lookup time, we throw only if the specific
    // requested nss is corrupted.
    stdx::unordered_set<NamespaceString> _invalidViewNames;
    // The flag indicates whether the catalog contains at least one view with an invalid namespace.
    // We cannot store invalid namespaces directly (parsing would fail), and there's no need to
    // since invalid namespaces cannot be queried anyway. We therefore store the catalog is
    // corrupted so that certain operations on views can still be prevented while allowing lookups
    // on valid views.
    bool _hasInvalidNss = false;
    bool _viewGraphNeedsRefresh = true;

    Stats _stats;
};

}  // namespace mongo
