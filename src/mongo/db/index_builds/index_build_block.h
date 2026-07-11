// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/index_builds/index_build_interceptor.h"
#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/index_builds/resumable_index_builds_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Interface for building a single index from an index spec and persisting its state to disk.
 */
class IndexBuildBlock {
    IndexBuildBlock(const IndexBuildBlock&) = delete;
    IndexBuildBlock& operator=(const IndexBuildBlock&) = delete;

public:
    /**
     * When resuming, whether the index table should be kept or recreated.
     */
    enum class IndexTableResumeBehavior {
        keep,
        recreate,
    };

    IndexBuildBlock(const NamespaceString& nss,
                    const BSONObj& spec,
                    IndexBuildMethodEnum method,
                    // The index build UUID is only required for persisting to the catalog.
                    boost::optional<UUID> indexBuildUUID);

    ~IndexBuildBlock();

    /**
     * Initializes a new entry for the index in the IndexCatalog.
     *
     * On success, holds pointer to newly created IndexCatalogEntry that can be accessed using
     * getEntry(). IndexCatalog will still own the entry.
     *
     * Must be called from within a `WriteUnitOfWork`
     */
    Status init(OperationContext* opCtx,
                Collection* collection,
                const IndexBuildInfo& indexBuildInfo,
                bool forRecovery);

    /**
     * Makes sure that an entry for the index was created at startup in the IndexCatalog. Returns
     * an error status if we are resuming from the bulk load phase and the index ident was unable
     * to be dropped or recreated in the storage engine.
     */
    Status initForResume(OperationContext* opCtx,
                         Collection* collection,
                         const IndexBuildInfo& indexBuildInfo,
                         IndexTableResumeBehavior behavior);

    /**
     * Marks the state of the index as 'ready' and commits the index to disk.
     *
     * Must be called from within a `WriteUnitOfWork`
     */
    void success(OperationContext* opCtx, Collection* collection);

    /**
     * Aborts the index build and removes any on-disk state where applicable.
     *
     * Must be called from within a `WriteUnitOfWork`
     */
    void fail(OperationContext* opCtx, Collection* collection);

    /**
     * Returns the IndexCatalogEntry that was created in init().
     *
     * This entry is owned by the IndexCatalog.
     */
    const IndexCatalogEntry* getEntry(OperationContext* opCtx,
                                      const CollectionPtr& collection) const;
    IndexCatalogEntry* getWritableEntry(OperationContext* opCtx, Collection* collection);

    /**
     * Returns the name of the index managed by this index builder.
     */
    std::string getIndexName() const {
        return std::string{_indexBuildInfo->getIndexName()};
    }

    const IndexBuildInfo& getIndexBuildInfo() const {
        return *_indexBuildInfo;
    }

    /**
     * Returns the index spec used to build this index.
     */
    const BSONObj& getSpec() const {
        return _spec;
    }

    static Status buildEmptyIndex(OperationContext* opCtx,
                                  Collection* collection,
                                  const IndexBuildInfo& indexBuildInfo);

    /**
     * Force-creates any backing tables still in deferred mode. Must be called before persisting
     * resume state so that the idents in the resume info exist on disk.
     */
    void createDeferredTables(OperationContext* opCtx) {
        if (_indexBuildInterceptor) {
            _indexBuildInterceptor->createDeferredTables(opCtx);
        }
    }

    void dropTemporaryTables(OperationContext* opCtx, StorageEngine::DropTime dropTime) {
        if (_indexBuildInterceptor) {
            _indexBuildInterceptor->dropTemporaryTables(opCtx, dropTime);
        }
    }

private:
    void _completeInit(OperationContext* opCtx, Collection* collection);

    const NamespaceString _nss;

    BSONObj _spec;
    IndexBuildMethodEnum _method;
    boost::optional<UUID> _buildUUID;

    boost::optional<IndexBuildInfo> _indexBuildInfo;
    std::string _indexNamespace;

    // TODO (SERVER-127702): This is shared_ptr only to satisfy weak_ptr's control block
    // requirement in IndexCatalogEntryImpl. IndexBuildBlock is the sole owner. Revert to
    // unique_ptr once we find a better way to represent the relationship between IndexBuildBlock
    // and IndexBuildInterceptor.
    std::shared_ptr<IndexBuildInterceptor> _indexBuildInterceptor;
};
}  // namespace mongo
