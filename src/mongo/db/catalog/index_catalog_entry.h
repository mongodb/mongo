// index_catalog_entry.h

/**
*    Copyright (C) 2013 10gen Inc.
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
#include "mongo/db/storage/snapshot_name.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"

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
    MONGO_DISALLOW_COPYING(IndexCatalogEntry);

public:
    IndexCatalogEntry(OperationContext* txn,
                      StringData ns,
                      CollectionCatalogEntry* collection,  // not owned
                      IndexDescriptor* descriptor,         // ownership passes to me
                      CollectionInfoCache* infoCache);     // not owned, optional

    ~IndexCatalogEntry();

    const std::string& ns() const {
        return _ns;
    }

    void init(std::unique_ptr<IndexAccessMethod> accessMethod);

    IndexDescriptor* descriptor() {
        return _descriptor;
    }
    const IndexDescriptor* descriptor() const {
        return _descriptor;
    }

    IndexAccessMethod* accessMethod() {
        return _accessMethod.get();
    }
    const IndexAccessMethod* accessMethod() const {
        return _accessMethod.get();
    }

    const Ordering& ordering() const {
        return _ordering;
    }

    const MatchExpression* getFilterExpression() const {
        return _filterExpression.get();
    }

    const CollatorInterface* getCollator() const {
        return _collator.get();
    }

    /// ---------------------

    const RecordId& head(OperationContext* txn) const;

    void setHead(OperationContext* txn, RecordId newHead);

    void setIsReady(bool newIsReady);

    HeadManager* headManager() const {
        return _headManager;
    }

    // --

    /**
     * Returns true if this index is multikey, and returns false otherwise.
     */
    bool isMultikey() const;

    /**
     * Returns the path components that cause this index to be multikey if this index supports
     * path-level multikey tracking, and returns an empty vector if path-level multikey tracking
     * isn't supported.
     *
     * If this index supports path-level multikey tracking but isn't multikey, then this function
     * returns a vector with size equal to the number of elements in the index key pattern where
     * each element in the vector is an empty set.
     */
    MultikeyPaths getMultikeyPaths(OperationContext* txn) const;

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
    void setMultikey(OperationContext* txn, const MultikeyPaths& multikeyPaths);

    // if this ready is ready for queries
    bool isReady(OperationContext* txn) const;

    /**
     * If return value is not boost::none, reads with majority read concern using an older snapshot
     * must treat this index as unfinished.
     */
    boost::optional<SnapshotName> getMinimumVisibleSnapshot() {
        return _minVisibleSnapshot;
    }

    void setMinimumVisibleSnapshot(SnapshotName name) {
        _minVisibleSnapshot = name;
    }

private:
    class SetMultikeyChange;
    class SetHeadChange;

    bool _catalogIsReady(OperationContext* txn) const;
    RecordId _catalogHead(OperationContext* txn) const;

    /**
     * Retrieves the multikey information associated with this index from '_collection',
     *
     * See CollectionCatalogEntry::isIndexMultikey() for more details.
     */
    bool _catalogIsMultikey(OperationContext* txn, MultikeyPaths* multikeyPaths) const;

    // -----

    std::string _ns;

    CollectionCatalogEntry* _collection;  // not owned here

    IndexDescriptor* _descriptor;  // owned here

    CollectionInfoCache* _infoCache;  // not owned here

    std::unique_ptr<IndexAccessMethod> _accessMethod;

    // Owned here.
    HeadManager* _headManager;
    std::unique_ptr<CollatorInterface> _collator;
    std::unique_ptr<MatchExpression> _filterExpression;

    // cached stuff

    Ordering _ordering;  // TODO: this might be b-tree specific
    bool _isReady;       // cache of NamespaceDetails info
    RecordId _head;      // cache of IndexDetails

    // Set to true if this index supports path-level multikey tracking.
    // '_indexTracksPathLevelMultikeyInfo' is effectively const after IndexCatalogEntry::init() is
    // called.
    bool _indexTracksPathLevelMultikeyInfo = false;

    // Set to true if this index is multikey. '_isMultikey' serves as a cache of the information
    // stored in the NamespaceDetails or KVCatalog.
    AtomicWord<bool> _isMultikey;

    // Controls concurrent access to '_indexMultikeyPaths'. We acquire this mutex rather than the
    // RESOURCE_METADATA lock as a performance optimization so that it is cheaper to detect whether
    // there is actually any path-level multikey information to update or not.
    mutable stdx::mutex _indexMultikeyPathsMutex;

    // Non-empty only if '_indexTracksPathLevelMultikeyInfo' is true.
    //
    // If non-empty, '_indexMultikeyPaths' is a vector with size equal to the number of elements
    // in the index key pattern. Each element in the vector is an ordered set of positions (starting
    // at 0) into the corresponding indexed field that represent what prefixes of the indexed field
    // causes the index to be multikey.
    MultikeyPaths _indexMultikeyPaths;

    // The earliest snapshot that is allowed to read this index.
    boost::optional<SnapshotName> _minVisibleSnapshot;
};

class IndexCatalogEntryContainer {
public:
    typedef std::vector<IndexCatalogEntry*>::const_iterator const_iterator;
    typedef std::vector<IndexCatalogEntry*>::const_iterator iterator;

    const_iterator begin() const {
        return _entries.vector().begin();
    }
    const_iterator end() const {
        return _entries.vector().end();
    }

    iterator begin() {
        return _entries.vector().begin();
    }
    iterator end() {
        return _entries.vector().end();
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
        _entries.mutableVector().push_back(entry);
    }

private:
    OwnedPointerVector<IndexCatalogEntry> _entries;
};
}
