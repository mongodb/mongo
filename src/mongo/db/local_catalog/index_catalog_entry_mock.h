/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"

namespace mongo {

class IndexDescriptor;

/**
 * This class comprises a mock IndexCatalogEntry for use in unit tests.
 */
class IndexCatalogEntryMock : public IndexCatalogEntry {
public:
    IndexCatalogEntryMock(OperationContext*,
                          const CollectionPtr&,
                          const std::string& ident,
                          IndexDescriptor&& descriptor,
                          bool)
        : _descriptor(descriptor), _ident(ident) {
        _descriptor.setEntry(this);
    }

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
        MONGO_UNREACHABLE;
    }

    void setAccessMethod(std::unique_ptr<IndexAccessMethod> accessMethod) final {
        MONGO_UNREACHABLE;
    }

    bool isHybridBuilding() const final {
        MONGO_UNREACHABLE;
    }

    IndexBuildInterceptor* indexBuildInterceptor() const final {
        MONGO_UNREACHABLE;
    }

    void setIndexBuildInterceptor(IndexBuildInterceptor* interceptor) final {
        MONGO_UNREACHABLE;
    }

    const Ordering& ordering() const final {
        MONGO_UNREACHABLE;
    }

    const MatchExpression* getFilterExpression() const final {
        MONGO_UNREACHABLE;
    }

    const CollatorInterface* getCollator() const final {
        MONGO_UNREACHABLE;
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
};

}  // namespace mongo
