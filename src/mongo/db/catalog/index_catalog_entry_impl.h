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

#pragma once

#include <boost/optional.hpp>
#include <string>

#include "mongo/bson/ordering.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/record_id.h"
#include "mongo/db/update_index_data.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"

namespace mongo {

class CollatorInterface;
class IndexAccessMethod;
class IndexDescriptor;
class MatchExpression;
class OperationContext;
class ExpressionContext;

class IndexCatalogEntryImpl : public IndexCatalogEntry {
    IndexCatalogEntryImpl(const IndexCatalogEntryImpl&) = delete;
    IndexCatalogEntryImpl& operator=(const IndexCatalogEntryImpl&) = delete;

public:
    IndexCatalogEntryImpl(OperationContext* opCtx,
                          const CollectionPtr& collection,
                          const std::string& ident,
                          std::unique_ptr<IndexDescriptor> descriptor,  // ownership passes to me
                          bool isFrozen);

    const std::string& getIdent() const final {
        return _ident;
    }

    std::shared_ptr<Ident> getSharedIdent() const final;

    void setIdent(std::shared_ptr<Ident> newIdent) final;

    IndexDescriptor* descriptor() final {
        return _descriptor.get();
    }
    const IndexDescriptor* descriptor() const final {
        return _descriptor.get();
    }

    IndexAccessMethod* accessMethod() const final {
        return _accessMethod.get();
    }

    void setAccessMethod(std::unique_ptr<IndexAccessMethod> accessMethod) final;

    bool isHybridBuilding() const final {
        return _indexBuildInterceptor != nullptr;
    }

    IndexBuildInterceptor* indexBuildInterceptor() const final {
        return _indexBuildInterceptor;
    }

    void setIndexBuildInterceptor(IndexBuildInterceptor* interceptor) final {
        _indexBuildInterceptor = interceptor;
    }

    const Ordering& ordering() const final;

    const MatchExpression* getFilterExpression() const final {
        return _filterExpression.get();
    }

    const CollatorInterface* getCollator() const final {
        return _collator.get();
    }

    NamespaceString getNSSFromCatalog(OperationContext* opCtx) const final;

    /// ---------------------

    void setIsReady(bool newIsReady) final;

    void setIsFrozen(bool newIsFrozen) final;

    void setDropped() final {
        _isDropped.store(true);
    }

    bool isDropped() const final {
        return _isDropped.load();
    }

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

    void forceSetMultikey(OperationContext* opCtx,
                          const CollectionPtr& coll,
                          bool isMultikey,
                          const MultikeyPaths& multikeyPaths) const final;

    bool isReady(OperationContext* opCtx) const final;

    bool isFrozen() const final;

    bool shouldValidateDocument() const final;

    bool isPresentInMySnapshot(OperationContext* opCtx) const final;

    bool isReadyInMySnapshot(OperationContext* opCtx) const final;

    const UpdateIndexData& getIndexedPaths() const final {
        return _indexedPaths;
    }

private:
    /**
     * Sets this index to be multikey when we are running inside a multi-document transaction.
     * Used by setMultikey() only.
     */
    Status _setMultikeyInMultiDocumentTransaction(OperationContext* opCtx,
                                                  const CollectionPtr& collection,
                                                  const MultikeyPaths& multikeyPaths) const;

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

    // -----

    const std::string _ident;

    std::unique_ptr<IndexDescriptor> _descriptor;  // owned here

    std::unique_ptr<IndexAccessMethod> _accessMethod;

    IndexBuildInterceptor* _indexBuildInterceptor = nullptr;  // not owned here

    std::unique_ptr<CollatorInterface> _collator;
    std::unique_ptr<MatchExpression> _filterExpression;
    // Special ExpressionContext used to evaluate the partial filter expression.
    boost::intrusive_ptr<ExpressionContext> _expCtxForFilter;

    // cached stuff

    const RecordId _catalogId;  // Location in the durable catalog of the collection entry
                                // containing this index entry.
    bool _isReady;              // cache of NamespaceDetails info
    bool _isFrozen;
    bool _shouldValidateDocument;
    AtomicWord<bool> _isDropped;  // Whether the index drop is committed.

    // Offset of this index within the Collection metadata.
    // Used to improve lookups without having to search for the index name
    // accessing the collection metadata.
    int _indexOffset;

    // Describes the paths indexed by this index.
    UpdateIndexData _indexedPaths;
};
}  // namespace mongo
