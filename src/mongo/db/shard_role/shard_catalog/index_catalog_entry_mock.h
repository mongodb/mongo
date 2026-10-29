// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/index/index_access_method.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/util/modules.h"

namespace mongo {

class IndexDescriptor;

/**
 * This class comprises a mock IndexCatalogEntry for use in unit tests.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] IndexCatalogEntryMock : public IndexCatalogEntry {
public:
    IndexCatalogEntryMock(OperationContext*,
                          const CollectionPtr&,
                          const std::string& ident,
                          IndexDescriptor&& descriptor,
                          bool isFrozen,
                          const MatchExpression* filter = nullptr,
                          const CollatorInterface* collator = nullptr)
        : _descriptor(descriptor), _ident(ident), _filter(filter), _collator(collator) {}

    const std::string& getIdent() const final {
        return _ident;
    }

    std::shared_ptr<Ident> getSharedIdent() const final {
        MONGO_UNREACHABLE;
    }

    void setIdent(std::shared_ptr<Ident> newIdent) final {
        MONGO_UNREACHABLE;
    }

    IndexDescriptor* descriptor() final {
        return &_descriptor;
    }
    const IndexDescriptor* descriptor() const final {
        return &_descriptor;
    }

    IndexAccessMethod* accessMethod() const final {
        return nullptr;
    }

    void setAccessMethod(std::unique_ptr<IndexAccessMethod> accessMethod) final {
        MONGO_UNREACHABLE;
    }

    std::shared_ptr<IndexBuildInterceptor> indexBuildInterceptor() const final {
        MONGO_UNREACHABLE;
    }

    void setIndexBuildInterceptor(std::shared_ptr<IndexBuildInterceptor> interceptor) final {
        MONGO_UNREACHABLE;
    }

    const Ordering& ordering() const final {
        MONGO_UNREACHABLE;
    }

    const MatchExpression* getFilterExpression() const final {
        return _filter;
    }

    const CollatorInterface* getCollator() const final {
        return _collator;
    }

    NamespaceString getNSSFromCatalog(OperationContext* opCtx) const final {
        MONGO_UNREACHABLE;
    }

    void setIsReady(bool newIsReady) final {
        MONGO_UNREACHABLE;
    }

    void setIsFrozen(bool newIsFrozen) final {
        MONGO_UNREACHABLE;
    }

    bool isMultikey(OperationContext* opCtx, const CollectionPtr& collection) const final {
        MONGO_UNREACHABLE;
    }

    MultikeyPaths getMultikeyPaths(OperationContext* opCtx,
                                   const CollectionPtr& collection) const final {
        MONGO_UNREACHABLE;
    }

    void setMultikey(OperationContext* opCtx,
                     const CollectionPtr& coll,
                     const KeyStringSet& multikeyMetadataKeys,
                     const MultikeyPaths& multikeyPaths) const final {
        MONGO_UNREACHABLE;
    }

    void setMultikeyForApplyOps(OperationContext* opCtx,
                                const CollectionPtr& coll,
                                const KeyStringSet& multikeyMetadataKeys,
                                const MultikeyPaths& multikeyPaths) const final {
        MONGO_UNREACHABLE;
    }

    void forceSetMultikey(OperationContext* opCtx,
                          const CollectionPtr& coll,
                          bool isMultikey,
                          const MultikeyPaths& multikeyPaths) const final {
        MONGO_UNREACHABLE;
    }

    bool isReady() const final {
        MONGO_UNREACHABLE;
    }

    bool isFrozen() const final {
        MONGO_UNREACHABLE;
    }

    bool shouldValidateDocument() const final {
        MONGO_UNREACHABLE;
    }

    const UpdateIndexData& getIndexedPaths() const final {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<const IndexCatalogEntry> getNormalizedEntry(
        OperationContext* opCtx, const CollectionPtr& coll) const final {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<const IndexCatalogEntry> cloneWithDifferentDescriptor(
        IndexDescriptor) const final {
        MONGO_UNREACHABLE;
    }

private:
    IndexDescriptor _descriptor;
    const std::string _ident;
    const MatchExpression* _filter;
    const CollatorInterface* _collator;
};

}  // namespace mongo
