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

#include <boost/optional.hpp>
#include <string>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/kv/kv_prefix.h"
#include "mongo/db/storage/snapshot_name.h"
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
class IndexDescriptor;
class MatchExpression;
class OperationContext;

class IndexCatalogEntry {
public:
    // This class represents the internal vtable for the (potentially polymorphic) implementation of
    // the `IndexCatalogEntry` class.  This allows us to expose an interface to this object without
    // requiring a dependency upon the implementation's definition library.
    class Impl {
    public:
        virtual ~Impl() = 0;

        virtual const std::string& ns() const = 0;

        virtual void init(std::unique_ptr<IndexAccessMethod> accessMethod) = 0;

        virtual IndexDescriptor* descriptor() = 0;

        virtual const IndexDescriptor* descriptor() const = 0;

        virtual IndexAccessMethod* accessMethod() = 0;

        virtual const IndexAccessMethod* accessMethod() const = 0;

        virtual const Ordering& ordering() const = 0;

        virtual const MatchExpression* getFilterExpression() const = 0;

        virtual const CollatorInterface* getCollator() const = 0;

        virtual const RecordId& head(OperationContext* opCtx) const = 0;

        virtual void setHead(OperationContext* opCtx, RecordId newHead) = 0;

        virtual void setIsReady(bool newIsReady) = 0;

        virtual HeadManager* headManager() const = 0;

        virtual bool isMultikey() const = 0;

        virtual MultikeyPaths getMultikeyPaths(OperationContext* opCtx) const = 0;

        virtual void setMultikey(OperationContext* opCtx, const MultikeyPaths& multikeyPaths) = 0;

        virtual bool isReady(OperationContext* opCtx) const = 0;

        virtual KVPrefix getPrefix() const = 0;

        virtual boost::optional<SnapshotName> getMinimumVisibleSnapshot() = 0;

        virtual void setMinimumVisibleSnapshot(SnapshotName name) = 0;
    };

private:
    static std::unique_ptr<Impl> makeImpl(IndexCatalogEntry* this_,
                                          OperationContext* opCtx,
                                          StringData ns,
                                          CollectionCatalogEntry* collection,
                                          std::unique_ptr<IndexDescriptor> descriptor,
                                          CollectionInfoCache* infoCache);

public:
    using factory_function_type = decltype(makeImpl);

    static void registerFactory(stdx::function<factory_function_type> factory);

    explicit IndexCatalogEntry(
        OperationContext* opCtx,
        StringData ns,
        CollectionCatalogEntry* collection,           // not owned
        std::unique_ptr<IndexDescriptor> descriptor,  // ownership passes to me
        CollectionInfoCache* infoCache);              // not owned, optional

    // Do not call this function.  It exists for use with test drivers that need to inject
    // alternative implementations.
    explicit IndexCatalogEntry(std::unique_ptr<Impl> impl) : _pimpl(std::move(impl)) {}

    inline ~IndexCatalogEntry() = default;

    inline IndexCatalogEntry(IndexCatalogEntry&&) = delete;
    inline IndexCatalogEntry& operator=(IndexCatalogEntry&&) = delete;

    inline const std::string& ns() const {
        return this->_impl().ns();
    }

    void init(std::unique_ptr<IndexAccessMethod> accessMethod);

    inline IndexDescriptor* descriptor() {
        return this->_impl().descriptor();
    }

    inline const IndexDescriptor* descriptor() const {
        return this->_impl().descriptor();
    }

    inline IndexAccessMethod* accessMethod() {
        return this->_impl().accessMethod();
    }

    inline const IndexAccessMethod* accessMethod() const {
        return this->_impl().accessMethod();
    }

    inline const Ordering& ordering() const {
        return this->_impl().ordering();
    }

    inline const MatchExpression* getFilterExpression() const {
        return this->_impl().getFilterExpression();
    }

    inline const CollatorInterface* getCollator() const {
        return this->_impl().getCollator();
    }

    /// ---------------------

    inline const RecordId& head(OperationContext* const opCtx) const {
        return this->_impl().head(opCtx);
    }

    inline void setHead(OperationContext* const opCtx, const RecordId newHead) {
        return this->_impl().setHead(opCtx, newHead);
    }

    inline void setIsReady(const bool newIsReady) {
        return this->_impl().setIsReady(newIsReady);
    }

    inline HeadManager* headManager() const {
        return this->_impl().headManager();
    }

    // --

    /**
     * Returns true if this index is multikey and false otherwise.
     */
    inline bool isMultikey() const {
        return this->_impl().isMultikey();
    }

    /**
     * Returns the path components that cause this index to be multikey if this index supports
     * path-level multikey tracking, and returns an empty vector if path-level multikey tracking
     * isn't supported.
     *
     * If this index supports path-level multikey tracking but isn't multikey, then this function
     * returns a vector with size equal to the number of elements in the index key pattern where
     * each element in the vector is an empty set.
     */
    inline MultikeyPaths getMultikeyPaths(OperationContext* const opCtx) const {
        return this->_impl().getMultikeyPaths(opCtx);
    }

    /**
     * Sets this index to be multikey. Information regarding which newly detected path components
     * cause this index to be multikey can also be specified.
     *
     * If this index doesn't support path-level multikey tracking, then 'multikeyPaths' is ignored.
     *
     * If this index supports path-level multikey tracking, then 'multikeyPaths' must be a vector
     * with size equal to the number of elements in the index key pattern. Additionally, at least
     * one path component of the indexed fields must cause this index to be multikey.
     */
    void setMultikey(OperationContext* const opCtx, const MultikeyPaths& multikeyPaths) {
        return this->_impl().setMultikey(opCtx, multikeyPaths);
    }

    // if this ready is ready for queries
    bool isReady(OperationContext* const opCtx) const {
        return this->_impl().isReady(opCtx);
    }

    KVPrefix getPrefix() const {
        return this->_impl().getPrefix();
    }

    /**
     * If return value is not boost::none, reads with majority read concern using an older snapshot
     * must treat this index as unfinished.
     */
    boost::optional<SnapshotName> getMinimumVisibleSnapshot() {
        return this->_impl().getMinimumVisibleSnapshot();
    }

    void setMinimumVisibleSnapshot(const SnapshotName name) {
        return this->_impl().setMinimumVisibleSnapshot(name);
    }

private:
    // This structure exists to give us a customization point to decide how to force users of this
    // class to depend upon the corresponding `index_catalog_entry.cpp` Translation Unit (TU).  All
    // public forwarding functions call `_impl(), and `_impl` creates an instance of this structure.
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

    std::unique_ptr<Impl> _pimpl;
};

class IndexCatalogEntryContainer {
public:
    typedef std::vector<std::unique_ptr<IndexCatalogEntry>>::const_iterator const_iterator;
    typedef std::vector<std::unique_ptr<IndexCatalogEntry>>::const_iterator iterator;

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


    unsigned size() const {
        return _entries.size();
    }

    // -----------------

    /**
     * Removes from _entries and returns the matching entry or NULL if none matches.
     */
    IndexCatalogEntry* release(const IndexDescriptor* desc);

    bool remove(const IndexDescriptor* desc) {
        IndexCatalogEntry* entry = release(desc);
        delete entry;
        return entry;
    }

    // pass ownership to EntryContainer
    void add(IndexCatalogEntry* entry) {
        _entries.push_back(std::unique_ptr<IndexCatalogEntry>{entry});
    }

private:
    std::vector<std::unique_ptr<IndexCatalogEntry>> _entries;
};
}  // namespace mongo
