
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

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/bson/ordering.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/kv/kv_prefix.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/debug_util.h"

namespace mongo {
class CollatorInterface;
class CollectionCatalogEntry;
class CollectionInfoCache;
class HeadManager;
class IndexAccessMethod;
class IndexBuildInterceptor;
class IndexDescriptor;
class MatchExpression;
class OperationContext;

class IndexCatalogEntry {
public:
    IndexCatalogEntry() = default;
    virtual ~IndexCatalogEntry() = default;

    inline IndexCatalogEntry(IndexCatalogEntry&&) = delete;
    inline IndexCatalogEntry& operator=(IndexCatalogEntry&&) = delete;

    virtual const std::string& ns() const = 0;

    virtual void init(std::unique_ptr<IndexAccessMethod> accessMethod) = 0;

    virtual IndexDescriptor* descriptor() = 0;

    virtual const IndexDescriptor* descriptor() const = 0;

    virtual IndexAccessMethod* accessMethod() = 0;

    virtual const IndexAccessMethod* accessMethod() const = 0;

    virtual bool isHybridBuilding() const = 0;

    virtual IndexBuildInterceptor* indexBuildInterceptor() = 0;

    virtual void setIndexBuildInterceptor(IndexBuildInterceptor* interceptor) = 0;

    virtual const Ordering& ordering() const = 0;

    virtual const MatchExpression* getFilterExpression() const = 0;

    virtual const CollatorInterface* getCollator() const = 0;

    /// ---------------------

    virtual const RecordId& head(OperationContext* const opCtx) const = 0;

    virtual void setHead(OperationContext* const opCtx, const RecordId newHead) = 0;

    virtual void setIsReady(const bool newIsReady) = 0;

    virtual HeadManager* headManager() const = 0;

    // --

    /**
     * Returns true if this index is multikey and false otherwise.
     */
    virtual bool isMultikey(OperationContext* opCtx) const = 0;

    /**
     * Returns the path components that cause this index to be multikey if this index supports
     * path-level multikey tracking, and returns an empty vector if path-level multikey tracking
     * isn't supported.
     *
     * If this index supports path-level multikey tracking but isn't multikey, then this function
     * returns a vector with size equal to the number of elements in the index key pattern where
     * each element in the vector is an empty set.
     */
    virtual MultikeyPaths getMultikeyPaths(OperationContext* const opCtx) const = 0;

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
    virtual void setMultikey(OperationContext* const opCtx, const MultikeyPaths& multikeyPaths) = 0;

    /**
     * TODO SERVER-36385 Remove this function: we don't set the feature tracker bit in 4.4
     * because 4.4 can only downgrade to 4.2 which can read long TypeBits.
     */
    virtual void setIndexKeyStringWithLongTypeBitsExistsOnDisk(OperationContext* const opCtx) = 0;

    // if this ready is ready for queries
    virtual bool isReady(OperationContext* const opCtx) const = 0;

    virtual KVPrefix getPrefix() const = 0;

    /**
     * If return value is not boost::none, reads with majority read concern using an older snapshot
     * must treat this index as unfinished.
     */
    virtual boost::optional<Timestamp> getMinimumVisibleSnapshot() = 0;

    virtual void setMinimumVisibleSnapshot(const Timestamp name) = 0;

    virtual void setNs(NamespaceString ns) = 0;
};

class IndexCatalogEntryContainer {
public:
    typedef std::vector<std::shared_ptr<IndexCatalogEntry>>::const_iterator const_iterator;
    typedef std::vector<std::shared_ptr<IndexCatalogEntry>>::const_iterator iterator;

    const_iterator begin() const {
        return _entries.begin();
    }

    const_iterator end() const {
        return _entries.end();
    }

    iterator begin() {
        return _entries.begin();
    }

    iterator end() {
        return _entries.end();
    }

    // TODO: these have to be SUPER SUPER FAST
    // maybe even some pointer trickery is in order
    const IndexCatalogEntry* find(const IndexDescriptor* desc) const;
    IndexCatalogEntry* find(const IndexDescriptor* desc);

    IndexCatalogEntry* find(const std::string& name);

    /**
     * Returns a pointer to the IndexCatalogEntry corresponding to 'desc', where the caller assumes
     * shared ownership of the catalog object. Returns null if the entry does not exist.
     */
    std::shared_ptr<IndexCatalogEntry> findShared(const IndexDescriptor* desc) const;

    unsigned size() const {
        return _entries.size();
    }

    // -----------------

    /**
     * Removes from _entries and returns the matching entry or NULL if none matches.
     */
    std::shared_ptr<IndexCatalogEntry> release(const IndexDescriptor* desc);

    bool remove(const IndexDescriptor* desc) {
        return static_cast<bool>(release(desc));
    }

    void add(std::shared_ptr<IndexCatalogEntry>&& entry) {
        _entries.push_back(std::move(entry));
    }

    std::vector<std::shared_ptr<const IndexCatalogEntry>> getAllEntries() const {
        return {_entries.begin(), _entries.end()};
    }

private:
    std::vector<std::shared_ptr<IndexCatalogEntry>> _entries;
};
}  // namespace mongo
