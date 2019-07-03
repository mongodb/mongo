/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/index/index_build_interceptor.h"

namespace mongo {

/**
 * Interface for building a single index from an index spec and persisting its state to disk.
 */
class IndexBuildBlock {
    IndexBuildBlock(const IndexBuildBlock&) = delete;
    IndexBuildBlock& operator=(const IndexBuildBlock&) = delete;

public:
    IndexBuildBlock(IndexCatalog* indexCatalog,
                    const NamespaceString& nss,
                    const BSONObj& spec,
                    IndexBuildMethod method);

    ~IndexBuildBlock();

    /**
     * Must be called before the object is destructed if init() has been called.
     * Cleans up the temporary tables that are created for an index build.
     *
     * Being called in a 'WriteUnitOfWork' has no effect.
     */
    void deleteTemporaryTables(OperationContext* opCtx);

    /**
     * Initializes a new entry for the index in the IndexCatalog.
     *
     * On success, holds pointer to newly created IndexCatalogEntry that can be accessed using
     * getEntry(). IndexCatalog will still own the entry.
     *
     * Must be called from within a `WriteUnitOfWork`
     */
    Status init(OperationContext* opCtx, Collection* collection);

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
    void fail(OperationContext* opCtx, const Collection* collection);

    /**
     * Returns the IndexCatalogEntry that was created in init().
     *
     * This entry is owned by the IndexCatalog.
     */
    IndexCatalogEntry* getEntry() {
        return _indexCatalogEntry;
    }

    /**
     * Returns the name of the index managed by this index builder.
     */
    const std::string& getIndexName() const {
        return _indexName;
    }

    /**
     * Returns the index spec used to build this index.
     */
    const BSONObj& getSpec() const {
        return _spec;
    }

private:
    IndexCatalog* const _indexCatalog;
    const NamespaceString _nss;

    BSONObj _spec;
    IndexBuildMethod _method;

    std::string _indexName;
    std::string _indexNamespace;

    IndexCatalogEntry* _indexCatalogEntry;

    std::unique_ptr<IndexBuildInterceptor> _indexBuildInterceptor;
};
}  // namespace mongo
