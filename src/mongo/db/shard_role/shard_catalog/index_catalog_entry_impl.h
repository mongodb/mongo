// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/update_index_data.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>

namespace mongo {

class CollatorInterface;
class ExpressionContext;
class IndexAccessMethod;
class MatchExpression;
class OperationContext;

class IndexCatalogEntryImpl : public IndexCatalogEntry {
public:
    IndexCatalogEntryImpl(OperationContext* opCtx,
                          const CollectionPtr& collection,
                          const std::string& ident,
                          IndexDescriptor&& descriptor,
                          bool isFrozen);

    ~IndexCatalogEntryImpl() override;

    const std::string& getIdent() const final {
        return _shared->_ident;
    }

    std::shared_ptr<Ident> getSharedIdent() const final;

    void setIdent(std::shared_ptr<Ident> newIdent) final;

    IndexDescriptor* descriptor() final {
        return &_descriptor;
    }
    const IndexDescriptor* descriptor() const final {
        return &_descriptor;
    }

    IndexAccessMethod* accessMethod() const final {
        return _shared->_accessMethod.get();
    }

    void setAccessMethod(std::unique_ptr<IndexAccessMethod> accessMethod) final;

    std::shared_ptr<IndexBuildInterceptor> indexBuildInterceptor() const final {
        return _indexBuildInterceptor.lock();
    }

    void setIndexBuildInterceptor(std::shared_ptr<IndexBuildInterceptor> interceptor) final {
        _indexBuildInterceptor = interceptor;
    }

    const Ordering& ordering() const final;

    const MatchExpression* getFilterExpression() const final {
        return _shared->_filterExpression.get();
    }

    const CollatorInterface* getCollator() const final {
        return _shared->_collator.get();
    }

    NamespaceString getNSSFromCatalog(OperationContext* opCtx) const final;

    /// ---------------------

    void setIsReady(bool newIsReady) final;

    void setIsFrozen(bool newIsFrozen) final;

    // --

    /**
     * Returns true if this index is multikey, and returns false otherwise.
     */
    bool isMultikey(OperationContext* opCtx, const CollectionPtr& collection) const final;

    /**
     * Returns the path components that cause this index to be multikey if this index supports
     * path-level multikey tracking, and returns an empty vector if path-level multikey tracking
     * isn't supported.
     *
     * If this index supports path-level multikey tracking but isn't multikey, then this function
     * returns a vector with size equal to the number of elements in the index key pattern where
     * each element in the vector is an empty set.
     */
    MultikeyPaths getMultikeyPaths(OperationContext* opCtx,
                                   const CollectionPtr& collection) const final;

    /**
     * Sets this index to be multikey. Information regarding which newly detected path components
     * cause this index to be multikey can also be specified.
     *
     * If this index doesn't support path-level multikey tracking, then 'multikeyPaths' is ignored.
     *
     * If this index supports path-level multikey tracking, then 'multikeyPaths' must be a vector
     * with size equal to the number of elements in the index key pattern. Additionally, at least
     * one path component of the indexed fields must cause this index to be multikey.
     *
     * If isTrackingMultikeyPathInfo() is set on the OperationContext's MultikeyPathTracker,
     * then after we confirm that we actually need to set the index as multikey, we will save the
     * namespace, index name, and multikey paths on the OperationContext rather than set the index
     * as multikey here.
     */
    void setMultikey(OperationContext* opCtx,
                     const CollectionPtr& coll,
                     const KeyStringSet& multikeyMetadataKeys,
                     const MultikeyPaths& multikeyPaths) const final;

    /**
     * Applies the setMultikeyMetadata op:'c' oplog entry. Only used by replication.
     *
     * It can run during steady-state replication, or recovery (startup recovery, replication
     * rollback, logical initial sync).
     * It needs not check FeatureFlagReplicateMultikeynessInTransactions: if replicating this oplog
     * entry, the feature flag is assumed to be enabled.
     */
    void setMultikeyForApplyOps(OperationContext* opCtx,
                                const CollectionPtr& coll,
                                const KeyStringSet& multikeyMetadataKeys,
                                const MultikeyPaths& multikeyPaths) const final;

    void forceSetMultikey(OperationContext* opCtx,
                          const CollectionPtr& coll,
                          bool isMultikey,
                          const MultikeyPaths& multikeyPaths) const final;

    bool isReady() const final {
        return _isReady;
    }

    bool isFrozen() const final;

    bool shouldValidateDocument() const final;

    const UpdateIndexData& getIndexedPaths() const final {
        return _shared->_indexedPaths;
    }

    std::unique_ptr<const IndexCatalogEntry> getNormalizedEntry(
        OperationContext* opCtx, const CollectionPtr& coll) const final;

    std::unique_ptr<const IndexCatalogEntry> cloneWithDifferentDescriptor(
        IndexDescriptor) const final;

private:
    /**
     * Sets this index to be multikey when we are running inside a multi-document transaction.
     * Used by setMultikey() only.
     */
    Status _setMultikeyInMultiDocumentTransaction(OperationContext* opCtx,
                                                  const CollectionPtr& collection,
                                                  const KeyStringSet& multikeyMetadataKeys,
                                                  const MultikeyPaths& multikeyPaths,
                                                  bool replicateMultikeyness) const;

    /**
     * Retrieves the multikey information associated with this index from '_collection',
     *
     * See CollectionCatalogEntry::isIndexMultikey() for more details.
     */
    bool _catalogIsMultikey(OperationContext* opCtx,
                            const CollectionPtr& collection,
                            MultikeyPaths* multikeyPaths) const;

    /**
     * Sets on-disk multikey flag for this index.
     */
    void _catalogSetMultikey(OperationContext* opCtx,
                             const CollectionPtr& collection,
                             const MultikeyPaths& multikeyPaths) const;

    /**
     * Inserts wildcard multikey metadata keys into the index and returns the number of newly
     * inserted keys. Also records wildcard multikey path metrics for any keys that were not
     * already present.
     */
    int64_t _insertWildcardMultikeyMetadataKeysAndCountNew(
        OperationContext* opCtx,
        const CollectionPtr& collection,
        const KeyStringSet& multikeyMetadataKeys) const;

    /**
     * Holder of shared state between IndexCatalogEntryImpl clones
     */
    struct SharedState : public RefCountable {
        SharedState(const std::string& ident, const RecordId& catalogId)
            : _ident(ident), _catalogId(catalogId) {}

        const std::string _ident;

        const RecordId _catalogId;  // Location in the durable catalog of the collection entry
                                    // containing this index entry.

        std::unique_ptr<IndexAccessMethod> _accessMethod;

        std::unique_ptr<CollatorInterface> _collator;
        std::unique_ptr<MatchExpression> _filterExpression;

        // Special ExpressionContext used to evaluate the partial filter expression.
        boost::intrusive_ptr<ExpressionContext> _expCtxForFilter;

        // Describes the paths indexed by this index.
        UpdateIndexData _indexedPaths;
    };

    std::weak_ptr<IndexBuildInterceptor> _indexBuildInterceptor;

    boost::intrusive_ptr<SharedState> _shared;

    IndexDescriptor _descriptor;

    bool _isReady;
    bool _isFrozen;
    bool _shouldValidateDocument;

    // Offset of this index within the Collection metadata. Used to improve lookups without having
    // to search for the index name accessing the collection metadata.
    int _indexOffset;
};
}  // namespace mongo
