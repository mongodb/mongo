/**
 *    Copyright (C) 2017 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/update_index_data.h"
#include "mongo/stdx/functional.h"

namespace mongo {
class Collection;
class IndexDescriptor;
class OperationContext;

/**
 * this is for storing things that you want to cache about a single collection
 * life cycle is managed for you from inside Collection.
 */
class CollectionInfoCache {
public:
    class Impl {
    public:
        virtual ~Impl() = 0;

        virtual PlanCache* getPlanCache() const = 0;

        virtual QuerySettings* getQuerySettings() const = 0;

        virtual const UpdateIndexData& getIndexKeys(OperationContext* opCtx) const = 0;

        virtual CollectionIndexUsageMap getIndexUsageStats() const = 0;

        virtual void init(OperationContext* opCtx) = 0;

        virtual void addedIndex(OperationContext* opCtx, const IndexDescriptor* desc) = 0;

        virtual void droppedIndex(OperationContext* opCtx, StringData indexName) = 0;

        virtual void clearQueryCache() = 0;

        virtual void notifyOfQuery(OperationContext* opCtx,
                                   const std::set<std::string>& indexesUsed) = 0;
    };

private:
    static std::unique_ptr<Impl> makeImpl(Collection* collection, const NamespaceString& ns);

public:
    using factory_function_type = decltype(makeImpl);

    static void registerFactory(stdx::function<factory_function_type> factory);

    explicit inline CollectionInfoCache(Collection* const collection, const NamespaceString& ns)
        : _pimpl(makeImpl(collection, ns)) {}

    inline ~CollectionInfoCache() = default;

    /**
     * Builds internal cache state based on the current state of the Collection's IndexCatalog.
     */
    inline void init(OperationContext* const opCtx) {
        return this->_impl().init(opCtx);
    }

    /**
     * Get the PlanCache for this collection.
     */
    inline PlanCache* getPlanCache() const {
        return this->_impl().getPlanCache();
    }

    /**
     * Get the QuerySettings for this collection.
     */
    inline QuerySettings* getQuerySettings() const {
        return this->_impl().getQuerySettings();
    }

    /* get set of index keys for this namespace.  handy to quickly check if a given
       field is indexed (Note it might be a secondary component of a compound index.)
    */
    inline const UpdateIndexData& getIndexKeys(OperationContext* const opCtx) const {
        return this->_impl().getIndexKeys(opCtx);
    }

    /**
     * Returns cached index usage statistics for this collection.  The map returned will contain
     * entry for each index in the collection along with both a usage counter and a timestamp
     * representing the date/time the counter is valid from.
     *
     * Note for performance that this method returns a copy of a StringMap.
     */
    inline CollectionIndexUsageMap getIndexUsageStats() const {
        return this->_impl().getIndexUsageStats();
    }

    /**
     * Register a newly-created index with the cache.  Must be called whenever an index is
     * built on the associated collection.
     *
     * Must be called under exclusive collection lock.
     */
    inline void addedIndex(OperationContext* const opCtx, const IndexDescriptor* const desc) {
        return this->_impl().addedIndex(opCtx, desc);
    }

    /**
     * Deregister a newly-dropped index with the cache.  Must be called whenever an index is
     * dropped on the associated collection.
     *
     * Must be called under exclusive collection lock.
     */
    inline void droppedIndex(OperationContext* const opCtx, const StringData indexName) {
        return this->_impl().droppedIndex(opCtx, indexName);
    }

    /**
     * Removes all cached query plans.
     */
    inline void clearQueryCache() {
        return this->_impl().clearQueryCache();
    }

    /**
     * Signal to the cache that a query operation has completed.  'indexesUsed' should list the
     * set of indexes used by the winning plan, if any.
     */
    inline void notifyOfQuery(OperationContext* const opCtx,
                              const std::set<std::string>& indexesUsed) {
        return this->_impl().notifyOfQuery(opCtx, indexesUsed);
    }

    std::unique_ptr<Impl> _pimpl;

    // This structure exists to give us a customization point to decide how to force users of this
    // class to depend upon the corresponding `collection_info_cache.cpp` Translation Unit (TU).
    // All public forwarding functions call `_impl(), and `_impl` creates an instance of this
    // structure.
    struct TUHook {
        static void hook() noexcept;

        explicit inline TUHook() noexcept {
            if (kDebugBuild)
                this->hook();
        }
    };

    inline const Impl& _impl() const {
        TUHook{};
        return *this->_pimpl;
    }

    inline Impl& _impl() {
        TUHook{};
        return *this->_pimpl;
    }
};
}  // namespace mongo
