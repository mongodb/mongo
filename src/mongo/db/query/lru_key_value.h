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

#include <list>
#include <memory>
#include <unordered_map>

#include "mongo/base/status.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * A key-value store structure with a least recently used (LRU) replacement
 * policy. The number of entries allowed in the kv-store is set as a constant
 * upon construction.
 *
 * Caveat:
 * This kv-store is NOT thread safe! The client to this utility is responsible
 * for protecting concurrent access to the LRU store if used in a threaded
 * context.
 *
 * Implemented as a doubly-linked list (std::list) with a hash map
 * (boost::unordered_map) for quickly locating the kv-store entries. The
 * add(), get(), and remove() operations are all O(1).
 *
 * The keys of generic type K map to values of type V*. The V*
 * pointers are owned by the kv-store.
 *
 * TODO: We could move this into the util/ directory and do any cleanup necessary to make it
 * fully general.
 */
template <class K, class V>
class LRUKeyValue {
public:
    LRUKeyValue(size_t maxSize) : _maxSize(maxSize), _currentSize(0){};

    ~LRUKeyValue() {
        clear();
    }

    typedef std::pair<K, V*> KVListEntry;

    typedef std::list<KVListEntry> KVList;
    typedef typename KVList::iterator KVListIt;
    typedef typename KVList::const_iterator KVListConstIt;

    typedef std::unordered_map<K, KVListIt> KVMap;
    typedef typename KVMap::const_iterator KVMapConstIt;

    /**
     * Add an (K, V*) pair to the store, where 'key' can
     * be used to retrieve value 'entry' from the store.
     *
     * Takes ownership of 'entry'.
     *
     * If 'key' already exists in the kv-store, 'entry' will
     * simply replace what is already there.
     *
     * The least recently used entry is evicted if the
     * kv-store is full prior to the add() operation.
     *
     * If an entry is evicted, it will be returned in
     * an unique_ptr for the caller to use before disposing.
     */
    std::unique_ptr<V> add(const K& key, V* entry) {
        // If the key already exists, delete it first.
        KVMapConstIt i = _kvMap.find(key);
        if (i != _kvMap.end()) {
            KVListIt found = i->second;
            delete found->second;
            _kvMap.erase(i);
            _kvList.erase(found);
            _currentSize--;
        }

        _kvList.push_front(std::make_pair(key, entry));
        _kvMap[key] = _kvList.begin();
        _currentSize++;

        // If the store has grown beyond its allowed size,
        // evict the least recently used entry.
        if (_currentSize > _maxSize) {
            V* evictedEntry = _kvList.back().second;
            invariant(evictedEntry);

            _kvMap.erase(_kvList.back().first);
            _kvList.pop_back();
            _currentSize--;
            invariant(_currentSize == _maxSize);

            // Pass ownership of evicted entry to caller.
            // If caller chooses to ignore this unique_ptr,
            // the evicted entry will be deleted automatically.
            return std::unique_ptr<V>(evictedEntry);
        }
        return std::unique_ptr<V>();
    }

    /**
     * Retrieve the value associated with 'key' from
     * the kv-store. The value is returned through the
     * out-parameter 'entryOut'.
     *
     * The kv-store retains ownership of 'entryOut', so
     * it should not be deleted by the caller.
     *
     * As a side effect, the retrieved entry is promoted
     * to the most recently used.
     */
    Status get(const K& key, V** entryOut) const {
        KVMapConstIt i = _kvMap.find(key);
        if (i == _kvMap.end()) {
            return Status(ErrorCodes::NoSuchKey, "no such key in LRU key-value store");
        }
        KVListIt found = i->second;
        V* foundEntry = found->second;

        // Promote the kv-store entry to the front of the list.
        // It is now the most recently used.
        _kvMap.erase(i);
        _kvList.erase(found);
        _kvList.push_front(std::make_pair(key, foundEntry));
        _kvMap[key] = _kvList.begin();

        *entryOut = foundEntry;
        return Status::OK();
    }

    /**
     * Remove the kv-store entry keyed by 'key'.
     */
    Status remove(const K& key) {
        KVMapConstIt i = _kvMap.find(key);
        if (i == _kvMap.end()) {
            return Status(ErrorCodes::NoSuchKey, "no such key in LRU key-value store");
        }
        KVListIt found = i->second;
        delete found->second;
        _kvMap.erase(i);
        _kvList.erase(found);
        _currentSize--;
        return Status::OK();
    }

    /**
     * Deletes all entries in the kv-store.
     */
    void clear() {
        for (KVListIt i = _kvList.begin(); i != _kvList.end(); i++) {
            delete i->second;
        }
        _kvList.clear();
        _kvMap.clear();
        _currentSize = 0;
    }

    /**
     * Returns true if entry is found in the kv-store.
     */
    bool hasKey(const K& key) const {
        return _kvMap.find(key) != _kvMap.end();
    }

    /**
     * Returns the number of entries currently in the kv-store.
     */
    size_t size() const {
        return _currentSize;
    }

    /**
     * TODO: The kv-store should implement its own iterator. Calling through to the underlying
     * iterator exposes the internals, and forces the caller to make a horrible type
     * declaration.
     */
    KVListConstIt begin() const {
        return _kvList.begin();
    }

    KVListConstIt end() const {
        return _kvList.end();
    }

private:
    // The maximum allowable number of entries in the kv-store.
    const size_t _maxSize;

    // The number of entries currently in the kv-store.
    size_t _currentSize;

    // (K, V*) pairs are stored in this std::list. They are sorted in order
    // of use, where the front is the most recently used and the back is the
    // least recently used.
    mutable KVList _kvList;

    // Maps from a key to the corresponding std::list entry.
    mutable KVMap _kvMap;
};

}  // namespace mongo
