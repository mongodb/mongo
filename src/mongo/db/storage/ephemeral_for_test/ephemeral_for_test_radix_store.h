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

#include <array>
#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <string.h>
#include <vector>

#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace ephemeral_for_test {


class merge_conflict_exception : std::exception {
    virtual const char* what() const noexcept {
        return "conflicting changes prevent successful merge";
    }
};

struct Metrics {
    AtomicWord<int64_t> totalMemory{0};
    AtomicWord<int32_t> totalNodes{0};
    AtomicWord<int32_t> totalChildren{0};

    void addMemory(size_t size) {
        totalMemory.fetchAndAdd(size);
    }

    void subtractMemory(size_t size) {
        totalMemory.fetchAndSubtract(size);
    }
};
enum class NodeType : uint8_t { LEAF, NODE4, NODE16, NODE48, NODE256 };

/**
 * RadixStore is a Trie data structure with the ability to share nodes among copies of trees to
 * minimize data duplication. Each node has a notion of ownership and if modifications are made to
 * non-uniquely owned nodes, they are copied to prevent dirtying the data for the other owners of
 * the node.
 */
template <class Key, class T>
class RadixStore {
    class Node;
    class Head;

    friend class RadixStoreTest;

public:
    using mapped_type = T;
    using value_type = std::pair<const Key, mapped_type>;
    using allocator_type = std::allocator<value_type>;
    using pointer = typename std::allocator_traits<allocator_type>::pointer;
    using const_pointer = typename std::allocator_traits<allocator_type>::const_pointer;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using uint8_t = std::uint8_t;
    using node_ptr = boost::intrusive_ptr<Node>;
    using head_ptr = boost::intrusive_ptr<Head>;

    static constexpr uint8_t maxByte = 255;

    template <class pointer_type, class reference_type>
    class radix_iterator {
        friend class RadixStore;

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = typename RadixStore::value_type;
        using difference_type = std::ptrdiff_t;
        using pointer = pointer_type;
        using reference = reference_type;

        radix_iterator() : _root(nullptr), _current(nullptr) {}

        ~radix_iterator() {
            updateTreeView(/*stopIfMultipleCursors=*/true);
        }

        radix_iterator& operator++() {
            repositionIfChanged();
            _findNext();
            return *this;
        }

        radix_iterator operator++(int) {
            repositionIfChanged();
            radix_iterator old = *this;
            ++*this;
            return old;
        }

        bool operator==(const radix_iterator& other) {
            repositionIfChanged();
            return _current == other._current;
        }

        bool operator!=(const radix_iterator& other) {
            repositionIfChanged();
            return _current != other._current;
        }

        reference operator*() {
            repositionIfChanged();
            return *(_current->_data);
        }

        const_pointer operator->() {
            repositionIfChanged();
            return &*(_current->_data);
        }

        /**
         * Attempts to restore the iterator on its former position in the updated tree if the tree
         * has changed.
         *
         * If the former position has been erased, the iterator finds the next node. It is
         * possible that no next node is available, so at that point the cursor is exhausted and
         * points to the end.
         */
        void repositionIfChanged() {
            if (!_current || !_root->_nextVersion)
                return;

            invariant(_current->_data);

            // Copy the key from _current before we move our _root reference.
            auto key = _current->_data->first;

            updateTreeView();
            RadixStore store(*_root);

            // Find the same or next node in the updated tree.
            _current = store.lower_bound(key)._current;
        }

    private:
        radix_iterator(const head_ptr& root) : _root(root), _current(nullptr) {}

        radix_iterator(const head_ptr& root, Node* current) : _root(root), _current(current) {}

        /**
         * This function traverses the tree to find the next left-most node with data. Modifies
         * '_current' to point to this node. It uses a pre-order traversal ('visit' the current
         * node itself then 'visit' the child subtrees from left to right).
         */
        void _findNext() {
            // If 'current' is a nullptr there is no next node to go to.
            if (_current == nullptr)
                return;

            // If 'current' is not a leaf, continue moving down and left in the tree until the next
            // node.
            if (!_current->isLeaf()) {
                _traverseLeftSubtree();
                return;
            }

            // Get path from root to '_current' since it is required to traverse up the tree.
            const Key& key = _current->_data->first;

            std::vector<Node*> context = RadixStore::_buildContext(key, _root.get());

            // 'node' should equal '_current' because that should be the last element in the stack.
            // Pop back once more to get access to its parent node. The parent node will enable
            // traversal through the neighboring nodes, and if there are none, the iterator will
            // move up the tree to continue searching for the next node with data.
            Node* node = context.back();
            context.pop_back();

            // In case there is no next node, set _current to be 'nullptr' which will mark the end
            // of the traversal.
            _current = nullptr;
            while (!context.empty()) {
                uint8_t oldKey = node->_trieKey.front();
                node = context.back();
                context.pop_back();

                // Check the children right of the node that the iterator was at already. This way,
                // there will be no backtracking in the traversal.
                bool res = _forEachChild(node, oldKey + 1, false, [this](node_ptr child) {
                    // If the current node has data, return it and exit. If not, find the
                    // left-most node with data in this sub-tree.
                    if (child->_data) {
                        _current = child.get();
                        return false;
                    }

                    _current = child.get();
                    _traverseLeftSubtree();
                    return false;
                });
                if (!res) {
                    return;
                }
            }
            return;
        }

        void _traverseLeftSubtree() {
            // This function finds the next left-most node with data under the sub-tree where
            // '_current' is root. However, it cannot return the root, and hence at least 1
            // iteration of the while loop is required.
            do {
                _forEachChild(_current, 0, false, [this](node_ptr child) {
                    _current = child.get();
                    return false;
                });
            } while (!_current->_data);
        }

        void updateTreeView(bool stopIfMultipleCursors = false) {
            while (_root && _root->_nextVersion) {
                if (stopIfMultipleCursors && _root->refCount() > 1)
                    return;

                bool clearPreviousFlag = _root->refCount() == 1;
                _root = _root->_nextVersion;
                if (clearPreviousFlag)
                    _root->_hasPreviousVersion = false;
            }
        }

        // "_root" is a pointer to the root of the tree over which this is iterating.
        head_ptr _root;

        // "_current" is the node that the iterator is currently on. _current->_data will never be
        // boost::none (unless it is within the process of tree traversal), and _current will be
        // become a nullptr once there are no more nodes left to iterate.
        Node* _current;
    };

    using iterator = radix_iterator<pointer, value_type&>;
    using const_iterator = radix_iterator<const_pointer, const value_type&>;

    template <class pointer_type, class reference_type>
    class reverse_radix_iterator {
        friend class RadixStore;
        friend class radix_iterator<pointer_type, reference_type&>;

    public:
        using value_type = typename RadixStore::value_type;
        using difference_type = std::ptrdiff_t;
        using pointer = pointer_type;
        using reference = reference_type;

        reverse_radix_iterator() : _root(nullptr), _current(nullptr) {}

        reverse_radix_iterator(const const_iterator& it) : _root(it._root), _current(it._current) {
            // If the iterator passed in is at the end(), then set _current to root which is
            // equivalent to rbegin(). Otherwise, move the iterator back one node, due to the fact
            // that the relationship &*r == &*(i-1) must be maintained for any reverse iterator 'r'
            // and forward iterator 'i'.
            if (_current == nullptr) {
                // If the tree is empty, then leave '_current' as nullptr.
                if (_root->isLeaf())
                    return;

                _current = _root.get();
                _traverseRightSubtree();
            } else {
                _findNextReverse();
            }
        }

        reverse_radix_iterator(const iterator& it) : _root(it._root), _current(it._current) {
            if (_current == nullptr) {
                _current = _root;
                _traverseRightSubtree();
            } else {
                _findNextReverse();
            }
        }

        ~reverse_radix_iterator() {
            updateTreeView(/*stopIfMultipleCursors=*/true);
        }

        reverse_radix_iterator& operator++() {
            repositionIfChanged();
            _findNextReverse();
            return *this;
        }

        reverse_radix_iterator operator++(int) {
            repositionIfChanged();
            reverse_radix_iterator old = *this;
            ++*this;
            return old;
        }

        bool operator==(const reverse_radix_iterator& other) {
            repositionIfChanged();
            return _current == other._current;
        }

        bool operator!=(const reverse_radix_iterator& other) {
            repositionIfChanged();
            return _current != other._current;
        }

        reference operator*() {
            repositionIfChanged();
            return *(_current->_data);
        }

        const_pointer operator->() {
            repositionIfChanged();
            return &*(_current->_data);
        }

        /**
         * Attempts to restore the iterator on its former position in the updated tree if the tree
         * has changed.
         *
         * If the former position has been erased, the iterator finds the next node. It is
         * possible that no next node is available, so at that point the cursor is exhausted and
         * points to the end.
         */
        void repositionIfChanged() {
            if (!_current || !_root->_nextVersion)
                return;

            invariant(_current->_data);

            // Copy the key from _current before we move our _root reference.
            auto key = _current->_data->first;

            updateTreeView();
            RadixStore store(*_root);

            // Find the same or next node in the updated tree.
            const_iterator it = store.lower_bound(key);

            // Couldn't find any nodes with key greater than currentKey in lower_bound().
            // So make _current point to the beginning, since rbegin() will point to the
            // previous node before key.
            if (!it._current)
                _current = store.rbegin()._current;
            else {
                _current = it._current;
                // lower_bound(), moved us one up in a forwards direction since the currentKey
                // didn't exist anymore, move one back.
                if (_current->_data->first > key)
                    _findNextReverse();
            }
        }

    private:
        reverse_radix_iterator(const head_ptr& root) : _root(root), _current(nullptr) {}

        reverse_radix_iterator(const head_ptr& root, Node* current)
            : _root(root), _current(current) {}

        void _findNextReverse() {
            // Reverse find iterates through the tree to find the "next" node containing data,
            // searching from right to left. Normally a pre-order traversal is used, but for
            // reverse, the ordering is to visit child nodes from right to left, then 'visit'
            // current node.
            if (_current == nullptr)
                return;

            const Key& key = _current->_data->first;

            std::vector<Node*> context = RadixStore::_buildContext(key, _root.get());
            Node* node = context.back();
            context.pop_back();

            // Due to the nature of the traversal, it will always be necessary to move up the tree
            // first because when the 'current' node was visited, it meant all its children had been
            // visited as well.
            uint8_t oldKey;
            _current = nullptr;
            while (!context.empty()) {
                oldKey = node->_trieKey.front();
                node = context.back();
                context.pop_back();

                // After moving up in the tree, continue searching for neighboring nodes to see if
                // they have data, moving from right to left.
                bool res = _forEachChild(node, oldKey - 1, true, [this](node_ptr child) {
                    // If there is a sub-tree found, it must have data, therefore it's necessary
                    // to traverse to the right most node.
                    _current = child.get();
                    _traverseRightSubtree();
                    return false;
                });
                if (!res) {
                    return;
                }

                // If there were no sub-trees that contained data, and the 'current' node has data,
                // it can now finally be 'visited'.
                if (node->_data) {
                    _current = node;
                    return;
                }
            }
        }

        void _traverseRightSubtree() {
            // This function traverses the given tree to the right most leaf of the subtree where
            // 'current' is the root.
            do {
                _forEachChild(_current, maxByte, true, [this](node_ptr child) {
                    _current = child.get();
                    return false;
                });
            } while (!_current->isLeaf());
        }

        void updateTreeView(bool stopIfMultipleCursors = false) {
            while (_root && _root->_nextVersion) {
                if (stopIfMultipleCursors && _root->refCount() > 1)
                    return;

                bool clearPreviousFlag = _root->refCount() == 1;
                _root = _root->_nextVersion;
                if (clearPreviousFlag)
                    _root->_hasPreviousVersion = false;
            }
        }

        // "_root" is a pointer to the root of the tree over which this is iterating.
        head_ptr _root;

        // "_current" is a the node that the iterator is currently on. _current->_data will never be
        // boost::none, and _current will be become a nullptr once there are no more nodes left to
        // iterate.
        Node* _current;
    };

    using reverse_iterator = reverse_radix_iterator<pointer, value_type&>;
    using const_reverse_iterator = reverse_radix_iterator<const_pointer, const value_type&>;

    // Constructors
    RadixStore() : _root(make_intrusive_node<Head>()) {}
    RadixStore(const RadixStore& other) : _root(make_intrusive_node<Head>(*(other._root))) {}
    RadixStore(const Head& other) : _root(make_intrusive_node<Head>(other)) {}

    friend void swap(RadixStore& first, RadixStore& second) {
        std::swap(first._root, second._root);
    }

    RadixStore(RadixStore&& other) {
        _root = std::move(other._root);
    }

    RadixStore& operator=(RadixStore other) {
        swap(*this, other);
        return *this;
    }

    // Equality
    bool operator==(const RadixStore& other) const {
        if (_root->_count != other._root->_count || _root->_dataSize != other._root->_dataSize)
            return false;

        RadixStore::const_iterator iter = this->begin();
        RadixStore::const_iterator other_iter = other.begin();

        while (iter != this->end()) {
            if (other_iter == other.end() || *iter != *other_iter) {
                return false;
            }

            iter++;
            other_iter++;
        }

        return other_iter == other.end();
    }

    bool operator!=(const RadixStore& other) const {
        return !(*this == other);
    }

    // Capacity
    bool empty() const {
        // Not relying on size() internally, as it may be updated late.
        return _root->isLeaf() && !_root->_data;
    }

    size_type size() const {
        return _root->_count;
    }

    size_type dataSize() const {
        return _root->_dataSize;
    }

    bool hasBranch() const {
        return _root->_nextVersion ? true : false;
    }

    // Metrics
    static int64_t totalMemory() {
        return _metrics.totalMemory.load();
    }

    static int32_t totalNodes() {
        return _metrics.totalNodes.load();
    }

    static float averageChildren() {
        auto totalNodes = _metrics.totalNodes.load();
        return totalNodes ? _metrics.totalChildren.load() / static_cast<float>(totalNodes) : 0;
    }

    // Modifiers
    void clear() noexcept {
        _root = make_intrusive_node<Head>();
    }

    std::pair<const_iterator, bool> insert(value_type&& value) {
        const Key& key = std::move(value).first;

        Node* node = _findNode(key);
        if (node != nullptr || key.size() == 0)
            return std::make_pair(end(), false);

        return _upsertWithCopyOnSharedNodes(key, std::move(value));
    }

    std::pair<const_iterator, bool> update(value_type&& value) {
        const Key& key = std::move(value).first;

        // Ensure that the item to be updated exists.
        auto item = RadixStore::find(key);
        if (item == RadixStore::end())
            return std::make_pair(item, false);

        // Setting the same value is a no-op
        if (item->second == value.second)
            return std::make_pair(item, false);

        return _upsertWithCopyOnSharedNodes(key, std::move(value));
    }

    /**
     * Returns whether the key was removed.
     */
    bool erase(const Key& key) {
        std::vector<std::pair<Node*, bool>> context;

        Node* prev = _root.get();
        int rootUseCount = _root->_hasPreviousVersion ? 2 : 1;
        bool isUniquelyOwned = _root->refCount() == rootUseCount;
        context.push_back(std::make_pair(prev, isUniquelyOwned));

        Node* node = nullptr;

        const uint8_t* charKey = reinterpret_cast<const uint8_t*>(key.data());
        size_t depth = prev->_depth + prev->_trieKey.size();
        while (depth < key.size()) {
            auto nodePtr = _findChild(prev, charKey[depth]);
            node = nodePtr.get();
            if (node == nullptr) {
                return false;
            }

            // If the prefixes mismatch, this key cannot exist in the tree.
            size_t p = _comparePrefix(node->_trieKey, charKey + depth, key.size() - depth);
            if (p != node->_trieKey.size()) {
                return false;
            }

            isUniquelyOwned = isUniquelyOwned && nodePtr->refCount() - 1 == 1;
            context.push_back(std::make_pair(node, isUniquelyOwned));
            depth = node->_depth + node->_trieKey.size();
            prev = node;
        }

        // Found the node, now remove it.

        Node* deleted = context.back().first;
        context.pop_back();

        // If the to-be deleted node is an internal node without data it is hidden from the user and
        // should not be deleted
        if (!deleted->_data)
            return false;

        if (!deleted->isLeaf()) {
            // The to-be deleted node is an internal node, and therefore updating its data to be
            // boost::none will "delete" it.
            _upsertWithCopyOnSharedNodes(key, boost::none);
            return true;
        }

        Node* parent = context.at(0).first;
        isUniquelyOwned = context.at(0).second;

        if (!isUniquelyOwned) {
            invariant(!_root->_nextVersion);
            invariant(_root->refCount() > rootUseCount);
            _root->_nextVersion = make_intrusive_node<Head>(*_root);
            _root = _root->_nextVersion;
            _root->_hasPreviousVersion = true;
            parent = _root.get();
        }

        size_t sizeOfRemovedData = node->_data->second.size();
        _root->_dataSize -= sizeOfRemovedData;
        _root->_count--;
        Node* prevParent = nullptr;
        for (size_t depth = 1; depth < context.size(); depth++) {
            Node* child = context.at(depth).first;
            isUniquelyOwned = context.at(depth).second;

            uint8_t childFirstChar = child->_trieKey.front();
            if (!isUniquelyOwned) {
                auto childCopy = _copyNode(child);
                _setChildPtr(parent, childFirstChar, childCopy);
                child = childCopy.get();
            }

            prevParent = parent;
            parent = child;
        }

        // Handle the deleted node, as it is a leaf.
        _setChildPtr(parent, deleted->_trieKey.front(), nullptr);

        // 'parent' may only have one child, in which case we need to evaluate whether or not
        // this node is redundant.
        if (auto newParent = _compressOnlyChild(prevParent, parent)) {
            parent = newParent;
        }

        // Don't shrink the root node.
        if (prevParent && parent->needShrink()) {
            parent = _shrink(prevParent, parent).get();
        }

        return true;
    }

    void merge3(const RadixStore& base, const RadixStore& other) {
        std::vector<Node*> context;
        std::vector<uint8_t> trieKeyIndex;
        difference_type deltaCount = _root->_count - base._root->_count;
        difference_type deltaDataSize = _root->_dataSize - base._root->_dataSize;

        invariant(this->_root->_trieKey.size() == 0 && base._root->_trieKey.size() == 0 &&
                  other._root->_trieKey.size() == 0);
        _merge3Helper(
            this->_root.get(), base._root.get(), other._root.get(), context, trieKeyIndex);
        _root->_count = other._root->_count + deltaCount;
        _root->_dataSize = other._root->_dataSize + deltaDataSize;
    }

    // Iterators
    const_iterator begin() const noexcept {
        if (_root->isLeaf() && !_root->_data)
            return end();

        Node* node = _begin(_root.get());
        return RadixStore::const_iterator(_root, node);
    }

    const_reverse_iterator rbegin() const noexcept {
        return const_reverse_iterator(end());
    }

    const_iterator end() const noexcept {
        return const_iterator(_root);
    }

    const_reverse_iterator rend() const noexcept {
        return const_reverse_iterator(_root);
    }

    const_iterator find(const Key& key) const {
        const_iterator it = RadixStore::end();

        Node* node = _findNode(key);
        if (node == nullptr)
            return it;
        else
            return const_iterator(_root, node);
    }

    const_iterator lower_bound(const Key& key) const {
        Node* node = _root.get();
        const uint8_t* charKey = reinterpret_cast<const uint8_t*>(key.data());
        std::vector<std::pair<Node*, uint8_t>> context;
        size_t depth = 0;

        // Traverse the path given the key to see if the node exists.
        while (depth < key.size()) {
            uint8_t idx = charKey[depth];

            // When we go back up the tree to search for the lower bound of key, always search to
            // the right of 'idx' so that we never search anything less than what the lower bound
            // would be.
            if (idx != UINT8_MAX)
                context.push_back(std::make_pair(node, idx + 1));

            auto nodePtr = _findChild(node, idx);
            if (!nodePtr)
                break;

            node = nodePtr.get();
            size_t mismatchIdx =
                _comparePrefix(node->_trieKey, charKey + depth, key.size() - depth);

            // There is a prefix mismatch, so we don't need to traverse anymore.
            if (mismatchIdx < node->_trieKey.size()) {
                // Check if the current key in the tree is greater than the one we are looking
                // for since it can't be equal at this point. It can be greater in two ways:
                // It can be longer or it can have a larger character at the mismatch index.
                uint8_t mismatchChar = charKey[mismatchIdx + depth];
                if (mismatchIdx == key.size() - depth ||
                    node->_trieKey[mismatchIdx] > mismatchChar) {
                    // If the current key is greater and has a value it is the lower bound.
                    if (node->_data)
                        return const_iterator(_root, node);

                    // If the current key has no value, place it in the context
                    // so that we can search its children.
                    context.push_back(std::make_pair(node, 0));
                }
                break;
            }

            depth = node->_depth + node->_trieKey.size();
        }

        if (depth == key.size()) {
            // If the node exists, then we can just return an iterator to that node.
            if (node->_data)
                return const_iterator(_root, node);

            // The search key is an exact prefix, so we need to search all of this node's
            // children.
            context.back() = std::make_pair(node, 0);
        }

        // The node with the provided key did not exist. Now we must find the next largest node, if
        // it exists.
        while (!context.empty()) {
            uint8_t idx = 0;
            std::tie(node, idx) = context.back();
            context.pop_back();

            bool exhausted = _forEachChild(node, idx, false, [&node, &context](node_ptr child) {
                node = child.get();
                if (!node->_data) {
                    // Need to search this node's children for the next largest node.
                    context.push_back(std::make_pair(node, 0));
                }
                return false;
            });
            if (!exhausted && node->_data) {
                // There exists a node with a key larger than the one given.
                return const_iterator(_root, node);
            }

            if (node->_trieKey.empty() && context.empty()) {
                // We have searched the root. There's nothing left to search.
                return end();
            }
        }

        // There was no node key at least as large as the one given.
        return end();
    }

    const_iterator upper_bound(const Key& key) const {
        const_iterator it = lower_bound(key);
        if (it == end())
            return it;

        if (it->first == key)
            return ++it;

        return it;
    }

    typename RadixStore::iterator::difference_type distance(iterator iter1, iterator iter2) {
        return std::distance(iter1, iter2);
    }

    typename RadixStore::iterator::difference_type distance(const_iterator iter1,
                                                            const_iterator iter2) {
        return std::distance(iter1, iter2);
    }

    std::string to_string_for_test() {
        return _walkTree(_root.get(), 0);
    }

private:
    /*
     * The base class of all other node types.
     */
    class Node {
        friend class RadixStore;
        friend class RadixStoreTest;

    public:
        Node() {
            addNodeMemory();
        }

        Node(NodeType type, std::vector<uint8_t> key) {
            _nodeType = type;
            _trieKey = key;
            addNodeMemory();
        }

        Node(const Node& other) {
            _nodeType = other._nodeType;
            _numChildren = other._numChildren;
            _depth = other._depth;
            _trieKey = other._trieKey;
            if (other._data) {
                addData(other._data.value());
            }
            // _refCount is initialized to 0 and not copied from other.

            addNodeMemory();
        }

        Node(Node&& other)
            : _nodeType(other._nodeType),
              _numChildren(other._numChildren),
              _depth(other._depth),
              _trieKey(std::move(other._trieKey)),
              _data(std::move(other._data)) {
            // _refCount is initialized to 0 and not copied from other.
            // The move constructor transfers the dynamic memory so only the static memory of Node
            // should be added.
            addNodeMemory(-static_cast<int64_t>(_trieKey.capacity()) *
                          static_cast<int64_t>(sizeof(uint8_t)));
        }

        virtual ~Node() {
            subtractNodeMemory();
        }

        bool isLeaf() const {
            return !_numChildren;
        }

        uint16_t numChildren() const {
            return _numChildren;
        }

        int refCount() const {
            return _refCount.load();
        }

        friend void intrusive_ptr_add_ref(Node* ptr) {
            ptr->_refCount.fetchAndAdd(1);
        }

        friend void intrusive_ptr_release(Node* ptr) {
            if (ptr->_refCount.fetchAndSubtract(1) == 1) {
                delete ptr;
            }
        }

    protected:
        NodeType _nodeType = NodeType::LEAF;
        uint16_t _numChildren = 0;
        unsigned int _depth = 0;
        std::vector<uint8_t> _trieKey;
        boost::optional<value_type> _data;
        AtomicWord<uint32_t> _refCount{0};

    private:
        bool needGrow() const {
            switch (_nodeType) {
                case NodeType::LEAF:
                    return true;
                case NodeType::NODE4:
                    return numChildren() == 4;
                case NodeType::NODE16:
                    return numChildren() == 16;
                case NodeType::NODE48:
                    return numChildren() == 48;
                case NodeType::NODE256:
                    return false;
            }
            MONGO_UNREACHABLE;
        }

        bool needShrink() const {
            switch (_nodeType) {
                case NodeType::LEAF:
                    return false;
                case NodeType::NODE4:
                    return numChildren() < 1;
                case NodeType::NODE16:
                    return numChildren() < 5;
                case NodeType::NODE48:
                    return numChildren() < 17;
                case NodeType::NODE256:
                    return numChildren() < 49;
            }
            MONGO_UNREACHABLE;
        }

        /*
         * Only adds non-empty value and handle the increment on memory metrics.
         */
        void addData(value_type value) {
            _data.emplace(value.first, value.second);
            _metrics.addMemory(value.first.capacity() + value.second.capacity());
        }

        void addNodeMemory(difference_type offset = 0) {
            _metrics.totalMemory.fetchAndAdd(sizeof(Node) + _trieKey.capacity() * sizeof(uint8_t) +
                                             offset);
            _metrics.totalNodes.fetchAndAdd(1);
        }

        void subtractNodeMemory() {
            size_t memUsage = sizeof(Node) + _trieKey.capacity() * sizeof(uint8_t);
            if (_data) {
                memUsage += _data->first.capacity() + _data->second.capacity();
            }
            _metrics.totalMemory.fetchAndSubtract(memUsage);
            _metrics.totalNodes.fetchAndSubtract(1);
        }
    };

    class Node4;
    class Node16;
    class Node48;
    class Node256;

    class NodeLeaf : public Node {
        friend class RadixStore;

    public:
        NodeLeaf(std::vector<uint8_t> key) : Node(NodeType::LEAF, key) {}
        NodeLeaf(const NodeLeaf& other) : Node(other) {}

        NodeLeaf(Node4&& other) : Node(std::move(other)) {
            this->_nodeType = NodeType::LEAF;
        }
    };

    /*
     * Node4 is used when the number of children is in the range of [1, 4].
     */
    class Node4 : public Node {
        friend class RadixStore;

    public:
        Node4(std::vector<uint8_t> key) : Node(NodeType::NODE4, key) {
            std::fill(_childKey.begin(), _childKey.end(), 0);
            addNodeMemory();
        }

        Node4(const Node4& other)
            : Node(other), _childKey(other._childKey), _children(other._children) {
            addNodeMemory();
        }

        /*
         * This move constructor should only be used when growing NodeLeaf to Node4.
         */
        Node4(NodeLeaf&& other) : Node(std::move(other)) {
            this->_nodeType = NodeType::NODE4;
            std::fill(_childKey.begin(), _childKey.end(), 0);
            addNodeMemory();
        }

        /*
         * This move constructor should only be used when shrinking Node16 to Node4.
         */
        Node4(Node16&& other) : Node(std::move(other)) {
            invariant(other.needShrink());
            this->_nodeType = NodeType::NODE4;
            std::fill(_childKey.begin(), _childKey.end(), 0);
            for (size_t i = 0; i < other.numChildren(); ++i) {
                _childKey[i] = other._childKey[i];
                _children[i] = std::move(other._children[i]);
            }
            addNodeMemory();
        }

        ~Node4() {
            _metrics.subtractMemory(sizeof(Node4) - sizeof(Node));
            _metrics.totalChildren.fetchAndSubtract(_children.size());
        }

    private:
        void addNodeMemory() {
            _metrics.totalMemory.fetchAndAdd(sizeof(Node4) - sizeof(Node));
            _metrics.totalChildren.fetchAndAdd(_children.size());
        }

        // The first bytes of each child's key is stored in a sorted order.
        std::array<uint8_t, 4> _childKey;
        std::array<node_ptr, 4> _children;
    };

    /*
     * Node16 is used when the number of children is in the range of [5, 16].
     */
    class Node16 : public Node {
        friend class RadixStore;

    public:
        Node16(std::vector<uint8_t> key) : Node(NodeType::NODE16, key) {
            std::fill(_childKey.begin(), _childKey.end(), 0);
            addNodeMemory();
        }

        Node16(const Node16& other)
            : Node(other), _childKey(other._childKey), _children(other._children) {
            addNodeMemory();
        }

        /*
         * This move constructor should only be used when growing Node4 to Node16.
         */
        Node16(Node4&& other) : Node(std::move(other)) {
            invariant(other.needGrow());
            this->_nodeType = NodeType::NODE16;
            auto n = other._childKey.size();
            for (size_t i = 0; i < n; ++i) {
                _childKey[i] = other._childKey[i];
                _children[i] = std::move(other._children[i]);
            }
            std::fill(_childKey.begin() + n, _childKey.end(), 0);
            addNodeMemory();
        }

        /*
         * This move constructor should only be used when shrinking Node48 to Node16.
         */
        Node16(Node48&& other) : Node(std::move(other)) {
            invariant(other.needShrink());
            this->_nodeType = NodeType::NODE16;
            std::fill(_childKey.begin(), _childKey.end(), 0);
            size_t cur = 0;
            for (size_t i = 0; i < other._childIndex.size(); ++i) {
                auto index = other._childIndex[i];
                if (index != maxByte) {
                    _childKey[cur] = i;
                    _children[cur++] = std::move(other._children[index]);
                }
            }
            addNodeMemory();
        }

        ~Node16() {
            _metrics.subtractMemory(sizeof(Node16) - sizeof(Node));
            _metrics.totalChildren.fetchAndSubtract(_children.size());
        }

    private:
        void addNodeMemory() {
            _metrics.totalMemory.fetchAndAdd(sizeof(Node16) - sizeof(Node));
            _metrics.totalChildren.fetchAndAdd(_children.size());
        }

        // _childKey is sorted ascendingly.
        std::array<uint8_t, 16> _childKey;
        std::array<node_ptr, 16> _children;
    };

    /*
     * Node48 is used when the number of children is in the range of [17, 48].
     */
    class Node48 : public Node {
        friend class RadixStore;

    public:
        Node48(std::vector<uint8_t> key) : Node(NodeType::NODE48, key) {
            std::fill(_childIndex.begin(), _childIndex.end(), maxByte);
            addNodeMemory();
        }

        Node48(const Node48& other)
            : Node(other), _childIndex(other._childIndex), _children(other._children) {
            addNodeMemory();
        }

        /*
         * This move constructor should only be used when growing Node16 to Node48.
         */
        Node48(Node16&& other) : Node(std::move(other)) {
            invariant(other.needGrow());
            this->_nodeType = NodeType::NODE48;
            std::fill(_childIndex.begin(), _childIndex.end(), maxByte);
            for (uint8_t i = 0; i < other._children.size(); ++i) {
                _childIndex[other._childKey[i]] = i;
                _children[i] = std::move(other._children[i]);
            }
            addNodeMemory();
        }

        /*
         * This move constructor should only be used when shrinking Node256 to Node48.
         */
        Node48(Node256&& other) : Node(std::move(other)) {
            invariant(other.needShrink());
            this->_nodeType = NodeType::NODE48;
            std::fill(_childIndex.begin(), _childIndex.end(), maxByte);
            size_t cur = 0;
            for (size_t i = 0; i < other._children.size(); ++i) {
                auto child = other._children[i];
                if (child) {
                    _childIndex[i] = cur;
                    _children[cur++] = child;
                }
            }
            addNodeMemory();
        }

        ~Node48() {
            _metrics.subtractMemory(sizeof(Node48) - sizeof(Node));
            _metrics.totalChildren.fetchAndSubtract(_children.size());
        }

    private:
        void addNodeMemory() {
            _metrics.totalMemory.fetchAndAdd(sizeof(Node48) - sizeof(Node));
            _metrics.totalChildren.fetchAndAdd(_children.size());
        }

        // A lookup table for child pointers. It has values from 0 to 48, where 0
        // represents empty, and all other index i maps to index i - 1 in _children.
        std::array<uint8_t, 256> _childIndex;
        std::array<node_ptr, 48> _children;
    };

    /*
     * Node256 is used when the number of children is in the range of [49, 256].
     */
    class Node256 : public Node {
        friend class RadixStore;

    public:
        Node256() {
            this->_nodeType = NodeType::NODE256;
            addNodeMemory();
        }

        Node256(std::vector<uint8_t> key) : Node(NodeType::NODE256, key) {
            addNodeMemory();
        }

        Node256(const NodeLeaf& other) : Node(other) {
            addNodeMemory();
        }

        Node256(const Node4& other) : Node(other) {
            this->_nodeType = NodeType::NODE256;
            for (size_t i = 0; i < other.numChildren(); ++i) {
                this->_children[other._childKey[i]] = other._children[i];
            }
            addNodeMemory();
        }

        Node256(const Node16& other) : Node(other) {
            this->_nodeType = NodeType::NODE256;
            for (size_t i = 0; i < other.numChildren(); ++i) {
                this->_children[other._childKey[i]] = other._children[i];
            }
            addNodeMemory();
        }

        Node256(const Node48& other) : Node(other) {
            this->_nodeType = NodeType::NODE256;
            for (size_t i = 0; i < other._childIndex.size(); ++i) {
                auto index = other._childIndex[i];
                if (index != maxByte) {
                    this->_children[i] = other._children[index];
                }
            }
            addNodeMemory();
        }

        Node256(const Node256& other) : Node(other), _children(other._children) {
            addNodeMemory();
        }

        /*
         * This move constructor should only be used when growing Node48 to Node256.
         */
        Node256(Node48&& other) : Node(std::move(other)) {
            invariant(other.needGrow());
            this->_nodeType = NodeType::NODE256;
            for (size_t i = 0; i < other._childIndex.size(); ++i) {
                auto index = other._childIndex[i];
                if (index != maxByte) {
                    _children[i] = std::move(other._children[index]);
                }
            }
            addNodeMemory();
        }

        ~Node256() {
            _metrics.subtractMemory(sizeof(Node256) - sizeof(Node));
            _metrics.totalChildren.fetchAndSubtract(_children.size());
        }

    private:
        void addNodeMemory() {
            _metrics.totalMemory.fetchAndAdd(sizeof(Node256) - sizeof(Node));
            _metrics.totalChildren.fetchAndAdd(_children.size());
        }

        std::array<node_ptr, 256> _children;
    };

    template <typename Node, typename... Args>
    boost::intrusive_ptr<Node> make_intrusive_node(Args&&... args) {
        auto ptr = new Node(std::forward<Args>(args)...);
        return boost::intrusive_ptr<Node>(ptr, true);
    }

    /**
     * Head is the root node of every RadixStore, it contains extra information used by cursors to
     * be able to see when the tree is modified and to respond to these changes by ensuring they are
     * not iterating over stale trees.
     */
    class Head : public Node256 {
        friend class RadixStore;

    public:
        Head() {
            addNodeMemory();
        }

        Head(std::vector<uint8_t> key) : Node256(key) {
            addNodeMemory();
        }

        Head(const Head& other) : Node256(other), _count(other._count), _dataSize(other._dataSize) {
            addNodeMemory();
        }

        // Copy constructor template for the five node types.
        template <typename NodeT>
        Head(const NodeT& other) : Node256(other) {
            addNodeMemory();
        }

        ~Head() {
            if (_nextVersion)
                _nextVersion->_hasPreviousVersion = false;
            _metrics.subtractMemory(sizeof(Head) - sizeof(Node256));
        }

        Head(Head&& other)
            : Node256(std::move(other)), _count(other._count), _dataSize(other._dataSize) {
            addNodeMemory();
        }

        bool hasPreviousVersion() const {
            return _hasPreviousVersion;
        }

    protected:
        // Forms a singly linked list of versions that is needed to reposition cursors after
        // modifications have been made.
        head_ptr _nextVersion;

        // While we have cursors that haven't been repositioned to the latest tree, this will be
        // true to help us understand when to copy on modifications due to the extra shared pointer
        // _nextVersion.
        bool _hasPreviousVersion = false;

    private:
        void addNodeMemory() {
            _metrics.totalMemory.fetchAndAdd(sizeof(Head) - sizeof(Node256));
        }

        size_type _count = 0;
        size_type _dataSize = 0;
    };

    /**
     * Return a string representation of all the nodes in this tree.
     * The string will look like:
     *
     *  food
     *   s
     *  bar
     *
     *  The number of spaces in front of each node indicates the depth
     *  at which the node lies.
     */
    std::string _walkTree(Node* node, int depth) {
        std::string ret;
        for (int i = 0; i < depth; i++) {
            ret.push_back(' ');
        }

        for (uint8_t ch : node->_trieKey) {
            ret.push_back(ch);
        }
        if (node->_data) {
            ret.push_back('*');
        }
        ret.push_back('\n');

        _forEachChild(node, 0, false, [this, &ret, depth](node_ptr child) {
            ret.append(_walkTree(child.get(), depth + 1));
            return true;
        });
        return ret;
    }

    /*
     * Helper function to iterate through _children array for different node types and execute the
     * given function on each child. The given function returns false when it intends to break the
     * iteration.
     * Returns false when the given function returns false on an element. Returns true
     * when the end of the array is reached.
     */
    static bool _forEachChild(Node* node,
                              uint16_t startKey,
                              bool reverse,
                              const std::function<bool(node_ptr)>& func) {
        if (startKey > maxByte || !node->_numChildren) {
            return true;
        }
        // Sets the step depending on the direction of iteration.
        int16_t step = reverse ? -1 : 1;
        switch (node->_nodeType) {
            case NodeType::LEAF:
                return true;
            case NodeType::NODE4: {
                Node4* node4 = static_cast<Node4*>(node);
                // Locates the actual starting position first.
                auto first = node4->_childKey.begin();
                auto numChildren = node4->numChildren();
                auto pos = std::find_if(first, first + numChildren, [&startKey](uint8_t keyByte) {
                    return keyByte >= startKey;
                });
                if (reverse) {
                    if (pos == first && *pos != startKey) {
                        return true;
                    }
                    if (pos == first + numChildren || *pos != startKey) {
                        --pos;
                    }
                }

                // No qualified elements.
                if (pos == first + numChildren) {
                    return true;
                }

                auto end = reverse ? -1 : numChildren;
                for (auto cur = pos - first; cur != end; cur += step) {
                    // All children within the range will not be null since they are stored
                    // consecutively.
                    if (!func(node4->_children[cur])) {
                        // Breaks the loop if the given function decides to.
                        return false;
                    }
                }
                return true;
            }
            case NodeType::NODE16: {
                Node16* node16 = static_cast<Node16*>(node);
                // Locates the actual starting position first.
                auto first = node16->_childKey.begin();
                auto numChildren = node16->numChildren();
                auto pos = std::find_if(first, first + numChildren, [&startKey](uint8_t keyByte) {
                    return keyByte >= startKey;
                });
                if (reverse) {
                    if (pos == first && *pos != startKey) {
                        return true;
                    }
                    if (pos == first + numChildren || *pos != startKey) {
                        --pos;
                    }
                }

                // No qualified elements.
                if (pos == first + numChildren) {
                    return true;
                }

                int16_t end = reverse ? -1 : numChildren;
                for (auto cur = pos - first; cur != end; cur += step) {
                    // All children within the range will not be null since they are stored
                    // consecutively.
                    if (!func(node16->_children[cur])) {
                        return false;
                    }
                }
                return true;
            }
            case NodeType::NODE48: {
                Node48* node48 = static_cast<Node48*>(node);
                size_t end = reverse ? -1 : node48->_childIndex.size();
                for (size_t cur = startKey; cur != end; cur += step) {
                    auto index = node48->_childIndex[cur];
                    if (index != maxByte && !func(node48->_children[index])) {
                        return false;
                    }
                }
                return true;
            }
            case NodeType::NODE256: {
                Node256* node256 = static_cast<Node256*>(node);
                size_t end = reverse ? -1 : node256->_children.size();
                for (size_t cur = startKey; cur != end; cur += step) {
                    auto child = node256->_children[cur];
                    if (child && !func(child)) {
                        return false;
                    }
                }
                return true;
            }
        }
        MONGO_UNREACHABLE;
    }

    /*
     * Gets the child mapped by the key for different node types. Node4 just does a simple linear
     * search. Node16 uses binary search. Node48 does one extra lookup with the index table. Node256
     * has direct mapping.
     */
    static node_ptr _findChild(const Node* node, uint8_t key) {
        switch (node->_nodeType) {
            case NodeType::LEAF:
                return node_ptr(nullptr);
            case NodeType::NODE4: {
                const Node4* node4 = static_cast<const Node4*>(node);
                auto start = node4->_childKey.begin();
                auto end = start + node4->numChildren();
                auto pos = std::find(start, end, key);
                return pos != end ? node4->_children[pos - start] : node_ptr(nullptr);
            }
            case NodeType::NODE16: {
                const Node16* node16 = static_cast<const Node16*>(node);
                auto start = node16->_childKey.begin();
                auto end = start + node16->numChildren();
                auto pos = std::find(start, end, key);
                return pos != end ? node16->_children[pos - start] : node_ptr(nullptr);
            }
            case NodeType::NODE48: {
                const Node48* node48 = static_cast<const Node48*>(node);
                auto index = node48->_childIndex[key];
                if (index != maxByte) {
                    return node48->_children[index];
                }
                return node_ptr(nullptr);
            }
            case NodeType::NODE256: {
                const Node256* node256 = static_cast<const Node256*>(node);
                return node256->_children[key];
            }
        }
        MONGO_UNREACHABLE;
    }

    Node* _findNode(const Key& key) const {
        const uint8_t* charKey = reinterpret_cast<const uint8_t*>(key.data());

        unsigned int depth = _root->_depth;
        unsigned int initialDepthOffset = depth;

        // If the root node's triekey is not empty then the tree is a subtree, and so we examine it.
        for (unsigned int i = 0; i < _root->_trieKey.size(); i++) {
            if (charKey[i + initialDepthOffset] != _root->_trieKey[i]) {
                return nullptr;
            }
            depth++;

            // Return node if entire trieKey matches.
            if (depth == key.size() && _root->_data &&
                (key.size() - initialDepthOffset) == _root->_trieKey.size()) {
                return _root.get();
            }
        }

        depth = _root->_depth + _root->_trieKey.size();
        uint8_t childFirstChar = charKey[depth];
        auto node = _findChild(_root.get(), childFirstChar);

        while (node != nullptr) {

            depth = node->_depth;

            size_t mismatchIdx =
                _comparePrefix(node->_trieKey, charKey + depth, key.size() - depth);
            if (mismatchIdx != node->_trieKey.size()) {
                return nullptr;
            } else if (mismatchIdx == key.size() - depth && node->_data) {
                return node.get();
            }

            depth = node->_depth + node->_trieKey.size();

            childFirstChar = charKey[depth];
            node = _findChild(node.get(), childFirstChar);
        }

        return nullptr;
    }

    /**
     * Makes a copy of the _root node if it isn't uniquely owned during an operation that will
     * modify the tree.
     *
     * The _root node wouldn't be uniquely owned only when there are cursors positioned on the
     * latest version of the tree. Cursors that are not yet repositioned onto the latest version of
     * the tree are not considered to be sharing the _root for modifying operations.
     */
    void _makeRootUnique() {
        int rootUseCount = _root->_hasPreviousVersion ? 2 : 1;

        if (_root->refCount() == rootUseCount)
            return;

        invariant(_root->refCount() > rootUseCount);
        // Copy the node on a modifying operation when the root isn't unique.

        // There should not be any _nextVersion set in the _root otherwise our tree would have
        // multiple HEADs.
        invariant(!_root->_nextVersion);
        _root->_nextVersion = make_intrusive_node<Head>(*_root);
        _root = _root->_nextVersion;
        _root->_hasPreviousVersion = true;
    }

    /*
     * Moves a smaller node's contents to a larger one. The method should only be called when the
     * node is full.
     */
    node_ptr _grow(Node* parent, Node* node) {
        invariant(node->_nodeType != NodeType::NODE256);
        auto key = node->_trieKey.front();
        switch (node->_nodeType) {
            case NodeType::NODE256:
                return nullptr;
            case NodeType::LEAF: {
                NodeLeaf* leaf = static_cast<NodeLeaf*>(node);
                auto newNode = make_intrusive_node<Node4>(std::move(*leaf));
                _setChildPtr(parent, key, newNode);
                return newNode;
            }
            case NodeType::NODE4: {
                Node4* node4 = static_cast<Node4*>(node);
                auto newNode = make_intrusive_node<Node16>(std::move(*node4));
                _setChildPtr(parent, key, newNode);
                return newNode;
            }
            case NodeType::NODE16: {
                Node16* node16 = static_cast<Node16*>(node);
                auto newNode = make_intrusive_node<Node48>(std::move(*node16));
                _setChildPtr(parent, key, newNode);
                return newNode;
            }
            case NodeType::NODE48: {
                Node48* node48 = static_cast<Node48*>(node);
                auto newNode = make_intrusive_node<Node256>(std::move(*node48));
                _setChildPtr(parent, key, newNode);
                return newNode;
            }
        }
        MONGO_UNREACHABLE;
    }

    node_ptr _shrink(Node* parent, Node* node) {
        invariant(node->_nodeType != NodeType::LEAF);
        auto key = node->_trieKey.front();
        switch (node->_nodeType) {
            case NodeType::LEAF:
                return nullptr;
            case NodeType::NODE4: {
                Node4* node4 = static_cast<Node4*>(node);
                auto newNode = make_intrusive_node<NodeLeaf>(std::move(*node4));
                _setChildPtr(parent, key, newNode);
                return newNode;
            }
            case NodeType::NODE16: {
                Node16* node16 = static_cast<Node16*>(node);
                auto newNode = make_intrusive_node<Node4>(std::move(*node16));
                _setChildPtr(parent, key, newNode);
                return newNode;
            }
            case NodeType::NODE48: {
                Node48* node48 = static_cast<Node48*>(node);
                auto newNode = make_intrusive_node<Node16>(std::move(*node48));
                _setChildPtr(parent, key, newNode);
                return newNode;
            }
            case NodeType::NODE256: {
                Node256* node256 = static_cast<Node256*>(node);
                auto newNode = make_intrusive_node<Node48>(std::move(*node256));
                _setChildPtr(parent, key, newNode);
                return newNode;
            }
        }
        MONGO_UNREACHABLE;
    }

    /**
     * _upsertWithCopyOnSharedNodes is a helper function to help manage copy on modification for the
     * tree. This function follows the path for the to-be modified node using the keystring. If at
     * any point, the path is no longer uniquely owned, the following nodes are copied to prevent
     * modification to other owner's data.
     *
     * 'key' is the key which can be followed to find the data.
     * 'value' is the data to be inserted or updated. It can be an empty value in which case it is
     * equivalent to removing that data from the tree.
     */
    std::pair<const_iterator, bool> _upsertWithCopyOnSharedNodes(
        const Key& key, boost::optional<value_type> value) {

        const uint8_t* charKey = reinterpret_cast<const uint8_t*>(key.data());

        int depth = _root->_depth + _root->_trieKey.size();
        uint8_t childFirstChar = charKey[depth];

        _makeRootUnique();

        Node* prevParent = nullptr;
        Node* prev = _root.get();
        node_ptr node = _findChild(prev, childFirstChar);
        while (node != nullptr) {
            if (node->refCount() - 1 > 1) {
                // Copy node on a modifying operation when it isn't owned uniquely.
                node = _copyNode(node.get());
                _setChildPtr(prev, childFirstChar, node);
            }

            // 'node' is uniquely owned at this point, so we are free to modify it.
            // Get the index at which node->_trieKey and the new key differ.
            size_t mismatchIdx =
                _comparePrefix(node->_trieKey, charKey + depth, key.size() - depth);

            // The keys mismatch, so we need to split this node.
            if (mismatchIdx != node->_trieKey.size()) {

                // Make a new node with whatever prefix is shared between node->_trieKey
                // and the new key. This will replace the current node in the tree.
                std::vector<uint8_t> newKey = _makeKey(node->_trieKey, 0, mismatchIdx);
                Node* newNode = _addChild(prev, newKey, boost::none, NodeType::NODE4);
                depth += mismatchIdx;
                const_iterator it(_root, newNode);
                if (key.size() - depth != 0) {
                    // Make a child with whatever is left of the new key.
                    newKey = _makeKey(charKey + depth, key.size() - depth);
                    Node* newChild = _addChild(newNode, newKey, value);
                    it = const_iterator(_root, newChild);
                } else {
                    // The new key is a prefix of an existing key, and has its own node, so we don't
                    // need to add any new nodes.
                    newNode->addData(value.value());
                }
                _root->_count++;
                _root->_dataSize += value->second.size();

                // Change the current node's trieKey and make a child of the new node.
                newKey = _makeKey(node->_trieKey, mismatchIdx, node->_trieKey.size() - mismatchIdx);
                _setChildPtr(newNode, newKey.front(), node);

                // Handle key size change to the new key.
                _metrics.subtractMemory(sizeof(uint8_t) *
                                        (node->_trieKey.capacity() - newKey.capacity()));
                node->_trieKey = newKey;
                node->_depth = newNode->_depth + newNode->_trieKey.size();

                return std::pair<const_iterator, bool>(it, true);
            } else if (mismatchIdx == key.size() - depth) {
                auto& data = node->_data;
                // The key already exists. If there's an element as well, account for its removal.
                if (data) {
                    _root->_count--;
                    _root->_dataSize -= data->second.size();
                    _metrics.subtractMemory(data->first.capacity() + data->second.capacity());
                }

                // Update an internal node.
                if (!value) {
                    data = boost::none;
                    auto keyByte = node->_trieKey.front();
                    if (_compressOnlyChild(prev, node.get())) {
                        node = _findChild(prev, keyByte);
                    }
                } else {
                    _root->_count++;
                    _root->_dataSize += value->second.size();
                    node->addData(value.value());
                }
                const_iterator it(_root, node.get());

                return std::pair<const_iterator, bool>(it, true);
            }

            depth = node->_depth + node->_trieKey.size();
            childFirstChar = charKey[depth];

            prevParent = prev;
            prev = node.get();
            node = _findChild(node.get(), childFirstChar);
        }

        // Add a completely new child to a node. The new key at this depth does not
        // share a prefix with any existing keys.
        std::vector<uint8_t> newKey = _makeKey(charKey + depth, key.size() - depth);
        if (prev->needGrow()) {
            prev = _grow(prevParent, prev).get();
        }
        Node* newNode = _addChild(prev, newKey, value);
        _root->_count++;
        _root->_dataSize += value->second.size();
        const_iterator it(_root, newNode);

        return std::pair<const_iterator, bool>(it, true);
    }

    /**
     * Return a uint8_t vector with the first 'count' characters of
     * 'old'.
     */
    std::vector<uint8_t> _makeKey(const uint8_t* old, size_t count) {
        std::vector<uint8_t> key;
        for (size_t i = 0; i < count; ++i) {
            uint8_t c = old[i];
            key.push_back(c);
        }
        return key;
    }

    /**
     * Return a uint8_t vector with the [pos, pos+count) characters from old.
     */
    std::vector<uint8_t> _makeKey(std::vector<uint8_t> old, size_t pos, size_t count) {
        std::vector<uint8_t> key;
        for (size_t i = pos; i < pos + count; ++i) {
            key.push_back(old[i]);
        }
        return key;
    }

    /*
     * Wraps around the copy constructor for different node types.
     */
    node_ptr _copyNode(Node* node) {
        switch (node->_nodeType) {
            case NodeType::LEAF: {
                NodeLeaf* leaf = static_cast<NodeLeaf*>(node);
                return make_intrusive_node<NodeLeaf>(*leaf);
            }
            case NodeType::NODE4: {
                Node4* node4 = static_cast<Node4*>(node);
                return make_intrusive_node<Node4>(*node4);
            }
            case NodeType::NODE16: {
                Node16* node16 = static_cast<Node16*>(node);
                return make_intrusive_node<Node16>(*node16);
            }
            case NodeType::NODE48: {
                Node48* node48 = static_cast<Node48*>(node);
                return make_intrusive_node<Node48>(*node48);
            }
            case NodeType::NODE256: {
                Node256* node256 = static_cast<Node256*>(node);
                return make_intrusive_node<Node256>(*node256);
            }
        }
        MONGO_UNREACHABLE;
    }

    head_ptr _makeHead(Node* node) {
        switch (node->_nodeType) {
            case NodeType::LEAF: {
                NodeLeaf* leaf = static_cast<NodeLeaf*>(node);
                return make_intrusive_node<Head>(*leaf);
            }
            case NodeType::NODE4: {
                Node4* node4 = static_cast<Node4*>(node);
                return make_intrusive_node<Head>(*node4);
            }
            case NodeType::NODE16: {
                Node16* node16 = static_cast<Node16*>(node);
                return make_intrusive_node<Head>(*node16);
            }
            case NodeType::NODE48: {
                Node48* node48 = static_cast<Node48*>(node);
                return make_intrusive_node<Head>(*node48);
            }
            case NodeType::NODE256: {
                Node256* node256 = static_cast<Node256*>(node);
                return make_intrusive_node<Head>(*node256);
            }
        }
        MONGO_UNREACHABLE;
    }

    /**
     * Add a child with trieKey 'key' and value 'value' to 'node'. The new child node created should
     * only be the type NodeLeaf or Node4.
     */
    Node* _addChild(Node* node,
                    std::vector<uint8_t> key,
                    boost::optional<value_type> value,
                    NodeType childType = NodeType::LEAF) {
        invariant(childType == NodeType::LEAF || childType == NodeType::NODE4);
        node_ptr newNode;
        if (childType == NodeType::LEAF) {
            newNode = make_intrusive_node<NodeLeaf>(key);
        } else {
            newNode = make_intrusive_node<Node4>(key);
        }
        newNode->_depth = node->_depth + node->_trieKey.size();
        if (value) {
            newNode->addData(value.value());
        }
        auto newNodeRaw = newNode.get();
        _setChildPtr(node, key.front(), std::move(newNode));
        return newNodeRaw;
    }

    /*
     * Inserts, updates, or deletes a child node at index and then maintains the key and children
     * array for Node4 and Node16 in _setChildPtr().
     */
    template <class Node4Or16>
    bool _setChildAtIndex(Node4Or16* node, node_ptr child, uint8_t key, size_t index) {
        // Adds to the end of the array.
        if (!node->_childKey[index] && !node->_children[index]) {
            node->_childKey[index] = key;
            node->_children[index] = child;
            ++node->_numChildren;
            return true;
        }
        if (node->_childKey[index] == key) {
            if (!child) {
                // Deletes a child and shifts
                auto n = node->numChildren();
                for (int j = index; j < n - 1; ++j) {
                    node->_childKey[j] = node->_childKey[j + 1];
                    node->_children[j] = node->_children[j + 1];
                }
                node->_childKey[n - 1] = 0;
                node->_children[n - 1] = nullptr;
                --node->_numChildren;
                return true;
            }
            node->_children[index] = child;
            return true;
        }
        if (node->_childKey[index] > key) {
            // _children is guaranteed not to be full.
            // Inserts to 'index' and shift larger keys to the right.
            for (size_t j = node->numChildren(); j > index; --j) {
                node->_childKey[j] = node->_childKey[j - 1];
                node->_children[j] = node->_children[j - 1];
            }
            node->_childKey[index] = key;
            node->_children[index] = child;
            ++node->_numChildren;
            return true;
        }
        return false;
    }

    /*
     * Sets the child pointer of 'key' to the 'newNode' in 'node'.
     */
    void _setChildPtr(Node* node, uint8_t key, node_ptr newNode) {
        switch (node->_nodeType) {
            case NodeType::LEAF:
                return;
            case NodeType::NODE4: {
                Node4* node4 = static_cast<Node4*>(node);
                auto start = node4->_childKey.begin();
                // Find the position of the first larger or equal key to insert.
                auto pos = std::find_if(start,
                                        start + node4->numChildren(),
                                        [&key](uint8_t keyByte) { return keyByte >= key; });
                _setChildAtIndex(node4, newNode, key, pos - start);
                return;
            }
            case NodeType::NODE16: {
                Node16* node16 = static_cast<Node16*>(node);
                auto start = node16->_childKey.begin();
                auto pos = std::find_if(start,
                                        start + node16->numChildren(),
                                        [&key](uint8_t keyByte) { return keyByte >= key; });
                _setChildAtIndex(node16, newNode, key, pos - start);
                return;
            }
            case NodeType::NODE48: {
                Node48* node48 = static_cast<Node48*>(node);
                auto index = node48->_childIndex[key];
                if (index != maxByte) {
                    // Pointer already exists. Delete or update it.
                    if (!newNode) {
                        node48->_childIndex[key] = maxByte;
                        --node48->_numChildren;
                    }
                    node48->_children[index] = newNode;
                    return;
                } else {
                    // Finds the first empty slot to insert the newNode.
                    for (size_t i = 0; i < node48->_children.size(); ++i) {
                        auto& child = node48->_children[i];
                        if (!child) {
                            child = newNode;
                            node48->_childIndex[key] = i;
                            ++node48->_numChildren;
                            return;
                        }
                    }
                }
                return;
            }
            case NodeType::NODE256: {
                Node256* node256 = static_cast<Node256*>(node);
                auto& child = node256->_children[key];
                if (!child) {
                    ++node256->_numChildren;
                } else if (!newNode) {
                    --node256->_numChildren;
                }
                child = newNode;
                return;
            }
        }
        MONGO_UNREACHABLE;
    }

    /**
     * This function traverses the tree starting at the provided node using the provided the
     * key. It returns the stack which is used in tree traversals for both the forward and
     * reverse iterators. Since both iterator classes use this function, it is declared
     * statically under RadixStore.
     *
     * This assumes that the key is present in the tree.
     */
    static std::vector<Node*> _buildContext(const Key& key, Node* node) {
        std::vector<Node*> context;
        context.push_back(node);

        const uint8_t* charKey = reinterpret_cast<const uint8_t*>(key.data());
        size_t depth = node->_depth + node->_trieKey.size();

        while (depth < key.size()) {
            node = _findChild(node, charKey[depth]).get();
            context.push_back(node);
            depth = node->_depth + node->_trieKey.size();
        }
        return context;
    }

    /**
     * Return the index at which 'key1' and 'key2' differ.
     * This function will interpret the bytes in 'key2' as unsigned values.
     */
    size_t _comparePrefix(std::vector<uint8_t> key1, const uint8_t* key2, size_t len2) const {
        size_t smaller = std::min(key1.size(), len2);

        size_t i = 0;
        for (; i < smaller; ++i) {
            uint8_t c = key2[i];
            if (key1[i] != c) {
                return i;
            }
        }
        return i;
    }

    /**
     * Compresses a child node into its parent if necessary. This is required when an erase results
     * in a node with no value and only one child.
     * Returns true if compression occurred and false otherwise.
     */
    Node* _compressOnlyChild(Node* parent, Node* node) {
        // Don't compress if this node is not of type Node4, has an actual value associated with it,
        // or doesn't have only one child.
        if (node->_nodeType != NodeType::NODE4 || node->_data || node->_trieKey.empty() ||
            node->numChildren() != 1) {
            return nullptr;
        }

        Node4* node4 = static_cast<Node4*>(node);
        node_ptr onlyChild = std::move(node4->_children[0]);
        node4->_childKey[0] = 0;
        node4->_numChildren = 0;
        auto oldCapacity = node4->_trieKey.capacity();

        for (char item : onlyChild->_trieKey) {
            node4->_trieKey.push_back(item);
        }
        _metrics.addMemory(sizeof(uint8_t) * (node4->_trieKey.capacity() - oldCapacity));
        if (onlyChild->_data) {
            node4->addData(onlyChild->_data.value());
        }
        _forEachChild(onlyChild.get(), 0, false, [this, &parent, &node](node_ptr child) {
            if (node->needGrow()) {
                node = _grow(parent, node).get();
            }
            auto keyByte = child->_trieKey.front();
            _setChildPtr(node, keyByte, std::move(child));
            return true;
        });

        if (node->needShrink()) {
            node = _shrink(parent, node).get();
        }

        return node;
    }

    /**
     * Rebuilds the context by replacing stale raw pointers with the new pointers. The pointers
     * can become stale when running an operation that copies the node on modification, like
     * insert or erase.
     */
    void _rebuildContext(std::vector<Node*>& context, std::vector<uint8_t>& trieKeyIndex) {
        Node* replaceNode = _root.get();
        context[0] = replaceNode;

        for (size_t node = 1; node < context.size(); node++) {
            replaceNode = _findChild(replaceNode, trieKeyIndex[node - 1]).get();
            context[node] = replaceNode;
        }
    }

    Node* _makeBranchUnique(std::vector<Node*>& context) {

        if (context.empty())
            return nullptr;

        // The first node should always be the root node.
        _makeRootUnique();
        context[0] = _root.get();

        // If the context only contains the root, and it was copied, return the new root.
        if (context.size() == 1)
            return _root.get();

        Node* node = nullptr;
        Node* prev = _root.get();

        // Create copies of the nodes until the leaf node.
        for (size_t idx = 1; idx < context.size(); idx++) {
            node = context[idx];

            auto next = _findChild(prev, node->_trieKey.front());
            if (next->refCount() - 1 > 1) {
                node_ptr nodeCopy = _copyNode(node);
                _setChildPtr(prev, nodeCopy->_trieKey.front(), nodeCopy);
                context[idx] = nodeCopy.get();
                prev = nodeCopy.get();
            } else {
                prev = next.get();
            }
        }

        return context.back();
    }

    /**
     * Resolves conflicts within subtrees due to the complicated structure of path-compressed radix
     * tries.
     */
    void _mergeResolveConflict(Node* current, Node* baseNode, Node* otherNode) {

        // Merges all differences between this and other, using base to determine whether operations
        // are allowed or should throw a merge conflict.
        RadixStore base, other, node;
        node._root = _makeHead(current);
        base._root = _makeHead(baseNode);
        other._root = _makeHead(otherNode);

        // Merges insertions and updates from the master tree into the working tree, if possible.
        for (const value_type& otherVal : other) {
            RadixStore::const_iterator baseIter = base.find(otherVal.first);
            RadixStore::const_iterator thisIter = node.find(otherVal.first);

            if (thisIter != node.end() && baseIter != base.end()) {
                // All three trees have a record of the node with the same key.
                if (thisIter->second == baseIter->second && baseIter->second != otherVal.second) {
                    // No changes occurred in the working tree, so the value in the master tree can
                    // be merged in cleanly.
                    this->update(RadixStore::value_type(otherVal));
                } else if (thisIter->second != baseIter->second &&
                           baseIter->second != otherVal.second) {
                    // Both the working copy and master nodes changed the same value at the same
                    // key. This results in a merge conflict.
                    throw merge_conflict_exception();
                } else if (thisIter->second != baseIter->second &&
                           thisIter->second == otherVal.second) {
                    // Both the working copy and master nodes are inserting the same value at the
                    // same key. But this is a merge conflict because if that operation was an
                    // increment, it's no different than a race condition on an unguarded variable.
                    throw merge_conflict_exception();
                }
            } else if (baseIter != base.end() && baseIter->second != otherVal.second) {
                // The working tree removed this node while the master updated the node, this
                // results in a merge conflict.
                throw merge_conflict_exception();
            } else if (thisIter != node.end()) {
                // Both the working copy and master tree are either inserting the same value or
                // different values at the same node, resulting in a merge conflict.
                throw merge_conflict_exception();
            } else if (thisIter == node.end() && baseIter == base.end()) {
                // The working tree and merge base do not have any record of this node. The node can
                // be merged in cleanly from the master tree.
                this->insert(RadixStore::value_type(otherVal));
            }
        }

        // Perform deletions from the master tree in the working tree, if possible.
        for (const value_type& baseVal : base) {
            RadixStore::const_iterator otherIter = other.find(baseVal.first);
            RadixStore::const_iterator thisIter = node.find(baseVal.first);

            if (otherIter == other.end()) {
                if (thisIter != node.end() && thisIter->second == baseVal.second) {
                    // Nothing changed between the working tree and merge base, so it is safe to
                    // perform the deletion that occurred in the master tree.
                    this->erase(baseVal.first);
                } else if (thisIter != node.end() && thisIter->second != baseVal.second) {
                    // The working tree made a change to the node while the master tree removed the
                    // node, resulting in a merge conflict.
                    throw merge_conflict_exception();
                }
            }
        }
    }

    /**
     * Merges elements from the master tree into the working copy if they have no presence in the
     * working copy, otherwise we throw a merge conflict.
     */
    void _mergeTwoBranches(Node* current, Node* otherNode) {

        RadixStore other, node;
        node._root = _makeHead(current);
        other._root = _makeHead(otherNode);

        for (const value_type& otherVal : other) {
            RadixStore::const_iterator thisIter = node.find(otherVal.first);

            if (thisIter != node.end())
                throw merge_conflict_exception();
            this->insert(RadixStore::value_type(otherVal));
        }
    }

    /**
     * Merges changes from base to other into current. Throws merge_conflict_exception if there are
     * merge conflicts.
     * It returns the updated current node and a boolean indicating that conflict resolution is
     * required after recursion
     */
    std::pair<Node*, bool> _merge3Helper(Node* current,
                                         const Node* base,
                                         const Node* other,
                                         std::vector<Node*>& context,
                                         std::vector<uint8_t>& trieKeyIndex) {
        context.push_back(current);

        // Root doesn't have a trie key.
        if (!current->_trieKey.empty())
            trieKeyIndex.push_back(current->_trieKey.at(0));

        auto hasParent = [](std::vector<Node*>& context) { return context.size() >= 2; };

        auto getParent = [&](std::vector<Node*>& context) {
            // We should never get here unless we are at sufficient depth already, so the invariant
            // should indeed actually hold. If coverity complains, treat as a false alarm.
            invariant(hasParent(context));
            return context[context.size() - 2];
        };

        auto currentHasBeenCompressed = [&]() {
            // This can only happen when conflict resolution erases nodes that causes compression on
            // the current node.
            return current->_trieKey.size() != other->_trieKey.size();
        };

        auto splitCurrentBeforeWriteIfNeeded = [&](Node* child) {
            // If current has not been compressed there's nothing to do
            if (!currentHasBeenCompressed())
                return child;

            // This can only happen if we've done previous writes to current so it should already be
            // unique and safe to write to.
            size_t mismatchIdx =
                _comparePrefix(current->_trieKey, other->_trieKey.data(), other->_trieKey.size());

            auto parent = getParent(context);
            auto key = current->_trieKey.front();
            auto newTrieKeyBegin = current->_trieKey.begin();
            auto newTrieKeyEnd = current->_trieKey.begin() + mismatchIdx;
            auto shared_current = std::move(_findChild(parent, key));
            auto newTrieKey = std::vector<uint8_t>(newTrieKeyBegin, newTrieKeyEnd);

            // Replace current with a new node with no data
            auto newNode = _addChild(parent, newTrieKey, boost::none, NodeType::NODE4);

            // Remove the part of the trieKey that is used by the new node.
            current->_trieKey.erase(current->_trieKey.begin(),
                                    current->_trieKey.begin() + mismatchIdx);
            _metrics.subtractMemory(sizeof(uint8_t) * mismatchIdx);
            current->_depth += mismatchIdx;

            // Add what was the current node as a child to the new internal node
            key = current->_trieKey.front();
            _setChildPtr(newNode, key, std::move(shared_current));

            // Update current pointer and context
            child = current;
            current = newNode;
            context.back() = current;
            return child;
        };

        bool resolveConflictNeeded = false;
        for (size_t key = 0; key < 256; ++key) {
            // Since _makeBranchUnique may make changes to the pointer addresses in recursive calls.
            current = context.back();

            Node* node = _findChild(current, key).get();
            Node* baseNode = _findChild(base, key).get();
            Node* otherNode = _findChild(other, key).get();

            if (!node && !baseNode && !otherNode)
                continue;

            bool unique = node != otherNode && node != baseNode;

            // If the current tree does not have this node, check if the other trees do.
            if (!node) {
                if (!baseNode && otherNode) {
                    splitCurrentBeforeWriteIfNeeded(nullptr);
                    // If base and node do NOT have this branch, but other does, then
                    // merge in the other's branch.
                    current = _makeBranchUnique(context);

                    if (current->needGrow() && hasParent(context)) {
                        current = _grow(getParent(context), current).get();
                    }

                    // Need to rebuild our context to have updated pointers due to the
                    // modifications that go on in _makeBranchUnique.
                    _rebuildContext(context, trieKeyIndex);
                    _setChildPtr(current, key, _findChild(other, key));
                } else if (!otherNode || (baseNode && baseNode != otherNode)) {
                    // Either the master tree and working tree remove the same branch, or the master
                    // tree updated the branch while the working tree removed the branch, resulting
                    // in a merge conflict.
                    throw merge_conflict_exception();
                }
            } else if (!unique) {
                if (baseNode && !otherNode && baseNode == node) {
                    node = splitCurrentBeforeWriteIfNeeded(node);

                    // Other has a deleted branch that must also be removed from current tree.
                    current = _makeBranchUnique(context);
                    _setChildPtr(current, key, nullptr);
                    if (current->needShrink() && hasParent(context)) {
                        current = _shrink(getParent(context), current).get();
                    }
                    _rebuildContext(context, trieKeyIndex);
                } else if (baseNode && otherNode && baseNode == node) {
                    node = splitCurrentBeforeWriteIfNeeded(node);

                    // If base and current point to the same node, then master changed.
                    current = _makeBranchUnique(context);
                    if (current->needGrow() && hasParent(context)) {
                        current = _grow(getParent(context), current).get();
                    }
                    _rebuildContext(context, trieKeyIndex);
                    _setChildPtr(current, key, _findChild(other, key));
                }
            } else if (baseNode && otherNode && baseNode != otherNode) {
                // If all three are unique and leaf nodes with different data, then it is a merge
                // conflict.
                if (node->isLeaf() && baseNode->isLeaf() && otherNode->isLeaf()) {
                    bool dataChanged = node->_data != baseNode->_data;
                    bool otherDataChanged = baseNode->_data != otherNode->_data;
                    if (dataChanged && otherDataChanged) {
                        // All three nodes have different data, that is a merge conflict
                        throw merge_conflict_exception();
                    }
                    if (otherDataChanged) {
                        // Only other changed the data. Take that node
                        current = _makeBranchUnique(context);
                        _rebuildContext(context, trieKeyIndex);
                        _setChildPtr(current, key, _findChild(other, key));
                    }
                    continue;
                }

                if (currentHasBeenCompressed()) {
                    resolveConflictNeeded = true;
                    break;
                }

                // If the keys and data are all the exact same, then we can keep recursing.
                // Otherwise, we manually resolve the differences element by element. The
                // structure of compressed radix tries makes it difficult to compare the
                // trees node by node, hence the reason for resolving these differences
                // element by element.
                bool resolveConflict =
                    !(node->_trieKey == baseNode->_trieKey &&
                      baseNode->_trieKey == otherNode->_trieKey && node->_data == baseNode->_data &&
                      baseNode->_data == otherNode->_data);
                if (!resolveConflict) {
                    std::tie(node, resolveConflict) =
                        _merge3Helper(node, baseNode, otherNode, context, trieKeyIndex);
                    if (node && !node->_data) {
                        // Drop if leaf node without data, that is not valid. Otherwise we might
                        // need to compress if we have only one child.
                        if (node->isLeaf()) {
                            _setChildPtr(current, key, nullptr);
                            // Don't shrink the root node.
                            if (current->needShrink() && hasParent(context)) {
                                current = _shrink(getParent(context), current).get();
                            }
                            _rebuildContext(context, trieKeyIndex);
                        } else {
                            if (auto compressedNode = _compressOnlyChild(current, node)) {
                                node = compressedNode;
                            }
                        }
                    }
                }
                if (resolveConflict) {
                    Node* nodeToResolve = node;
                    if (hasParent(context)) {
                        if (auto compressed = _compressOnlyChild(getParent(context), current)) {
                            current = compressed;
                            nodeToResolve = current;
                        }
                    }
                    _mergeResolveConflict(nodeToResolve, baseNode, otherNode);
                    _rebuildContext(context, trieKeyIndex);
                    // If we compressed above, resolving the conflict can result in erasing current.
                    // Break out of the recursion as there is nothing more to do.
                    if (!context.back())
                        break;
                }
            } else if (baseNode && !otherNode) {
                // Throw a write conflict since current has modified a branch but master has
                // removed it.
                throw merge_conflict_exception();
            } else if (!baseNode && otherNode) {
                // Both the working tree and master added branches that were nonexistent in base.
                // This requires us to resolve these differences element by element since the
                // changes may not be conflicting.
                if (currentHasBeenCompressed()) {
                    resolveConflictNeeded = true;
                    break;
                }

                _mergeTwoBranches(node, otherNode);
                _rebuildContext(context, trieKeyIndex);
            }
        }

        current = context.back();
        context.pop_back();
        if (!trieKeyIndex.empty())
            trieKeyIndex.pop_back();

        return std::make_pair(current, resolveConflictNeeded);
    }

    Node* _begin(Node* root) const noexcept {
        Node* node = root;
        while (!node->_data) {
            _forEachChild(node, 0, false, [&node](node_ptr child) {
                node = child.get();
                return false;
            });
        }
        return node;
    }

    head_ptr _root = nullptr;
    static Metrics _metrics;
};

template <class Key, class T>
Metrics RadixStore<Key, T>::_metrics;

using StringStore = RadixStore<std::string, std::string>;
}  // namespace ephemeral_for_test
}  // namespace mongo
