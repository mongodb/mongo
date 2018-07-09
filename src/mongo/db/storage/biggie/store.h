/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <exception>
#include <map>

namespace mongo {
namespace biggie {

class merge_conflict_exception : std::exception {
    virtual const char* what() const noexcept {
        return "conflicting changes prevent successful merge";
    }
};

template <class Key, class T>
class Store {
private:
    std::map<Key, T> map;

public:
    using mapped_type = T;
    using value_type = std::pair<const Key, mapped_type>;
    using allocator_type = std::allocator<value_type>;
    using pointer = typename std::allocator_traits<allocator_type>::pointer;
    using const_pointer = typename std::allocator_traits<allocator_type>::const_pointer;
    using size_type = std::size_t;

    template <class pointer_type, class reference_type, class iter_type>
    class store_iterator {
    private:
        friend class Store;
        iter_type iter;
        store_iterator(iter_type iter) : iter(iter) {}

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = typename Store::value_type;
        using difference_type = std::ptrdiff_t;
        using pointer = pointer_type;
        using reference = reference_type;

        store_iterator& operator++() {
            ++this->iter;
            return *this;
        }

        store_iterator operator++(int) {
            store_iterator old = *this;
            ++this->iter;
            return old;
        }

        store_iterator& operator--() {
            --this->iter;
            return *this;
        }

        store_iterator operator--(int) {
            store_iterator old = *this;
            --this->iter;
            return old;
        }

        // Non member equality

        friend bool operator==(const store_iterator& lhs, const store_iterator& rhs) {
            return lhs.iter == rhs.iter;
        }

        friend bool operator!=(const store_iterator& lhs, const store_iterator& rhs) {
            return !(lhs.iter == rhs.iter);
        }

        reference operator*() const {
            return *this->iter;
        }

        pointer operator->() const {
            return &(*this->iter);
        }
    };

    using iterator = store_iterator<pointer, value_type&, typename std::map<Key, T>::iterator>;
    using const_iterator =
        store_iterator<const_pointer, const value_type&, typename std::map<Key, T>::const_iterator>;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using reverse_const_iterator = std::reverse_iterator<const_iterator>;

    // Constructors

    Store(const Store& other) = default;
    Store() = default;

    ~Store() = default;

    // Non member equality

    friend bool operator==(const Store& lhs, const Store& rhs) {
        return lhs.map == rhs.map;
    }

    // Capacity

    bool empty() const {
        return map.empty();
    }

    // Number of nodes
    size_type size() const {
        return map.size();
    }

    // Size of mapped data in store
    size_type dataSize() const {
        size_type s = size_type(0);
        for (const value_type val : *this) {
            s += val.second.size();
        }

        return s;
    }

    // Modifiers

    void clear() noexcept {
        map.clear();
    }

    std::pair<iterator, bool> insert(value_type&& value) {
        auto res = this->map.insert(value);
        iterator iter = iterator(res.first);
        return std::pair<iterator, bool>(iter, res.second);
    }

    size_type erase(const Key& key) {
        return map.erase(key);
    }

    /**
     * Returns a Store that has all changes from both 'this' and 'other' compared to base.
     * Throws merge_conflict_exception if there are merge conflicts.
     */
    Store merge3(const Store& base, const Store& other) const {
        Store store;

        // Merges all differences between this and base, along with modifications from other.
        for (const Store::value_type val : *this) {
            Store::const_iterator baseIter = base.find(val.first);
            Store::const_iterator otherIter = other.find(val.first);

            if (baseIter != base.end() && otherIter != other.end()) {
                if (val.second != baseIter->second && otherIter->second != baseIter->second) {
                    // Throws exception if there are conflicting modifications.
                    throw merge_conflict_exception();
                }

                if (val.second != baseIter->second) {
                    // Merges non-conflicting insertions from this.
                    store.insert(Store::value_type(val));
                } else {
                    // Merges non-conflicting modifications from other or no modifications.
                    store.insert(Store::value_type(*otherIter));
                }
            } else if (baseIter != base.end() && otherIter == other.end()) {
                if (val.second != baseIter->second) {
                    // Throws exception if modifications from this conflict with deletions from
                    // other.
                    throw merge_conflict_exception();
                }
            } else if (baseIter == base.end()) {
                if (otherIter != other.end()) {
                    // Throws exception if insertions from this conflict with insertions from other.
                    throw merge_conflict_exception();
                }

                // Merges insertions from this.
                store.insert(Store::value_type(val));
            }
        }

        // Merges insertions and deletions from other.
        for (const Store::value_type otherVal : other) {
            Store::const_iterator baseIter = base.find(otherVal.first);
            Store::const_iterator thisIter = this->find(otherVal.first);

            if (baseIter == base.end()) {
                // Merges insertions from other.
                store.insert(Store::value_type(otherVal));
            } else if (thisIter == this->end() && otherVal.second != baseIter->second) {
                // Throws exception if modifications from this conflict with deletions from other.
                throw merge_conflict_exception();
            }
        }

        return store;
    }

    // Iterators

    iterator begin() noexcept {
        return iterator(this->map.begin());
    }

    iterator end() noexcept {
        return iterator(this->map.end());
    }

    reverse_iterator rbegin() noexcept {
        return reverse_iterator(end());
    }

    reverse_iterator rend() noexcept {
        return reverse_iterator(begin());
    }

    const_iterator begin() const noexcept {
        return const_iterator(this->map.begin());
    }

    const_iterator end() const noexcept {
        return const_iterator(this->map.end());
    }

    reverse_const_iterator rbegin() const noexcept {
        return reverse_const_iterator(end());
    }

    reverse_const_iterator rend() const noexcept {
        return reverse_const_iterator(begin());
    }

    // Look up

    iterator find(const Key& key) {
        return iterator(this->map.find(key));
    }

    const_iterator find(const Key& key) const {
        return const_iterator(this->map.find(key));
    }

    iterator lower_bound(const Key& key) {
        return iterator(this->map.lower_bound(key));
    }

    const_iterator lower_bound(const Key& key) const {
        return const_iterator(this->map.lower_bound(key));
    }

    iterator upper_bound(const Key& key) {
        return iterator(this->map.upper_bound(key));
    }

    const_iterator upper_bound(const Key& key) const {
        return const_iterator(this->map.upper_bound(key));
    }

    // std::distance

    typename iterator::difference_type distance(iterator iter1, iterator iter2) {
        return typename iterator::difference_type(std::distance(iter1.iter, iter2.iter));
    };

    typename iterator::difference_type distance(const_iterator iter1, const_iterator iter2) const {
        return typename iterator::difference_type(std::distance(iter1.iter, iter2.iter));
    };
};
}
}
