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

#include <array>
#include <boost/optional.hpp>
#include <exception>
#include <iostream>
#include <memory>
#include <string.h>
#include <vector>

namespace mongo {
namespace biggie {

class merge_conflict_exception : std::exception {
    virtual const char* what() const noexcept {
        return "conflicting changes prevent successful merge";
    }
};

/**
 * RadixStore is a Trie data structure with the ability to share nodes among copies of trees to
 * minimize data duplication. Each node has a notion of ownership and if modifications are made to
 * non-uniquely owned nodes, they are copied to prevent dirtying the data for the other owners of
 * the node.
 */
template <class Key, class T>
class RadixStore {
    class Node;

public:
    using mapped_type = T;
    using value_type = std::pair<const Key, mapped_type>;
    using allocator_type = std::allocator<value_type>;
    using pointer = typename std::allocator_traits<allocator_type>::pointer;
    using const_pointer = typename std::allocator_traits<allocator_type>::const_pointer;
    using size_type = std::size_t;
    using uint8_t = std::uint8_t;

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

        radix_iterator(const radix_iterator& other)
            : _root(other._root), _current(other._current) {}

        ~radix_iterator() = default;

        radix_iterator& operator++() {
            _findNext();
            return *this;
        }

        radix_iterator operator++(int) {
            radix_iterator old = *this;
            ++*this;
            return old;
        }

        radix_iterator& operator=(const radix_iterator& other) = default;

        bool operator==(const radix_iterator& other) const {
            return this->_current == other._current;
        }

        bool operator!=(const radix_iterator& other) const {
            return this->_current != other._current;
        }

        reference operator*() const {
            return *(_current->data);
        }

        const_pointer operator->() {
            return &*(_current->data);
        }

    private:
        radix_iterator(const std::shared_ptr<Node>& root) : _root(root), _current(nullptr) {}

        radix_iterator(const std::shared_ptr<Node>& root, Node* current)
            : _root(root), _current(current){};

        /**
        * This function traverses the tree to find the next left-most node with data. Modifies
        * '_current' to point to this node. It uses a pre-order traversal ('visit' the current
        * node itself then 'visit' the child subtrees from left to right ).
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
            Key key = _current->data->first;
            std::vector<Node*> context = RadixStore::_buildContext(key, _root.get());

            // 'node' should equal '_current' because that should be the last element in the stack.
            // Pop back once more to get access to it's parent node. The parent node will enable
            // traversal through the neighboring nodes, and if there are none, the iterator will
            // move up the tree to continue searching for the next node with data.
            Node* node = context.back();
            context.pop_back();

            // In case there is no next node, set _current to be 'nullptr' which will mark the end
            // of the traversal.
            _current = nullptr;
            while (!context.empty()) {
                uint8_t oldKey = node->trieKey;
                node = context.back();
                context.pop_back();

                // Check the children right of the node that the iterator was at already. This way,
                // there will be no backtracking in the traversal.
                for (auto iter = oldKey + 1 + node->children.begin(); iter != node->children.end();
                     ++iter) {

                    // If the node has a child, then the sub-tree must have a node with data that
                    // has not yet been visited.
                    if (*iter != nullptr) {

                        // If the current node has data, return it and exit. If not, continue
                        // following the nodes to find the next one with data. It is necessary to go
                        // to the left-most node in this sub-tree.
                        if ((*iter)->data != boost::none) {
                            _current = iter->get();
                            return;
                        }
                        _current = iter->get();
                        _traverseLeftSubtree();
                        return;
                    }
                }
            }
            return;
        }

        void _traverseLeftSubtree() {
            // This function finds the next left-most node with data under the sub-tree where
            // '_current' is root. However, it cannot return the root, and hence at least 1
            // iteration of the while loop is required.
            do {
                for (auto child : _current->children) {
                    if (child != nullptr) {
                        _current = child.get();
                        break;
                    }
                }
            } while (_current->data == boost::none);
        }

        // "_root" is a copy of the root of the tree over which this is iterating.
        std::shared_ptr<Node> _root;

        // "_current" is the node that the iterator is currently on. _current->data will never be
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
                _current = _root.get();
                _traverseRightSubtree();
            } else {
                _findNextReverse();
            }
        }

        reverse_radix_iterator(const reverse_radix_iterator& other)
            : _root(other._root), _current(other._current) {}

        reverse_radix_iterator& operator=(const reverse_radix_iterator& other) = default;

        ~reverse_radix_iterator() = default;

        reverse_radix_iterator& operator++() {
            _findNextReverse();
            return *this;
        }

        reverse_radix_iterator operator++(int) {
            reverse_radix_iterator old = *this;
            ++*this;
            return old;
        }

        bool operator==(const reverse_radix_iterator& other) const {
            return this->_current == other._current;
        }

        bool operator!=(const reverse_radix_iterator& other) const {
            return this->_current != other._current;
        }

        reference operator*() const {
            return *(_current->data);
        }

        const_pointer operator->() {
            return &*(_current->data);
        }


    private:
        reverse_radix_iterator(const std::shared_ptr<Node>& root)
            : _root(root), _current(nullptr) {}
        reverse_radix_iterator(const std::shared_ptr<Node>& root, Node* current)
            : _root(root), _current(current) {}

        void _findNextReverse() {
            // Reverse find iterates through the tree to find the "next" node containing data,
            // searching from right to left. Normally a pre-order traversal is used, but for
            // reverse, the ordering is to visit child nodes from right to left, then 'visit'
            // current node.
            if (_current == nullptr)
                return;

            Key key = _current->data->first;

            std::vector<Node*> context = RadixStore::_buildContext(key, _root.get());
            Node* node = context.back();
            context.pop_back();

            // Due to the nature of the traversal, it will always be necessary to move up the tree
            // first because when the 'current' node was visited, it meant all its children had been
            // visited as well.
            uint8_t oldKey;
            _current = nullptr;
            while (!context.empty()) {
                oldKey = node->trieKey;
                node = context.back();
                context.pop_back();

                // After moving up in the tree, continue searching for neighboring nodes to see if
                // they have data, moving from right to left.
                for (int i = oldKey - 1; i >= 0; i--) {
                    if (node->children[i] != nullptr) {
                        // If there is a sub-tree found, it must have data, therefore it's necessary
                        // to traverse to the right most node.
                        _current = node->children[i].get();
                        _traverseRightSubtree();
                        return;
                    }
                }

                // If there were no sub-trees that contained data, and the 'current' node has data,
                // it can now finally be 'visited'.
                if (node->data != boost::none) {
                    _current = node;
                    return;
                }
            }
        }

        void _traverseRightSubtree() {
            // This function traverses the given tree to the right most leaf of the subtree where
            // 'current' is the root.
            do {
                for (auto iter = _current->children.rbegin(); iter != _current->children.rend();
                     ++iter) {
                    if (*iter != nullptr) {
                        _current = iter->get();
                        break;
                    }
                }
            } while (!_current->isLeaf());
        }

        // "_root" is a copy of the root of the tree over which this is iterating.
        std::shared_ptr<Node> _root;

        // "_current" is a the node that the iterator is currently on. _current->data will never be
        // boost::none, and _current will be become a nullptr once there are no more nodes left to
        // iterate.
        Node* _current;
    };

    using reverse_iterator = reverse_radix_iterator<pointer, value_type&>;
    using const_reverse_iterator = reverse_radix_iterator<const_pointer, const value_type&>;

    // Constructor
    RadixStore(const RadixStore& other) {
        _root = other._root;
        _numElems = other._numElems;
        _sizeElems = other._sizeElems;
    }

    RadixStore() {
        _root = std::make_shared<RadixStore::Node>('\0');
        _numElems = 0;
        _sizeElems = 0;
    }

    ~RadixStore() = default;

    // Equality
    bool operator==(const RadixStore& other) const {
        RadixStore::const_iterator iter = this->begin();
        RadixStore::const_iterator other_iter = other.begin();

        while (iter != this->end()) {
            if (*iter != *other_iter) {
                return false;
            }

            iter++;
            other_iter++;
        }

        return other_iter == other.end();
    }

    // Capacity
    bool empty() const {
        return _numElems == 0;
    }

    size_type size() const {
        return _numElems;
    }

    size_type dataSize() const {
        return _sizeElems;
    }

    // Modifiers
    void clear() noexcept {
        _root = std::make_shared<Node>('\0');
        _numElems = 0;
        _sizeElems = 0;
    }

    std::pair<const_iterator, bool> insert(value_type&& value) {
        Key key = value.first;
        mapped_type m = value.second;

        Node* item = _findNode(key);
        if (item != nullptr || key.size() == 0)
            return std::make_pair(end(), false);

        auto result = _upsertWithCopyOnSharedNodes(key, std::move(value));
        if (result.second) {
            _numElems++;
            _sizeElems += m.size();
        }

        return result;
    }

    std::pair<const_iterator, bool> update(value_type&& value) {
        Key key = value.first;
        mapped_type m = value.second;

        // Ensure that the item to be updated exists.
        auto item = RadixStore::find(key);
        if (item == RadixStore::end())
            return std::make_pair(item, false);

        size_t sizeOfRemovedNode = item->second.size();
        auto result = _upsertWithCopyOnSharedNodes(key, std::move(value));
        if (result.second) {
            _sizeElems -= sizeOfRemovedNode;
            _sizeElems += m.size();
        }

        return result;
    }

    size_type erase(const Key& key) {
        std::vector<std::pair<Node*, bool>> context;

        std::shared_ptr<Node> node = _root;
        bool isUniquelyOwned = _root.use_count() - 1 == 1;
        context.push_back(std::make_pair(node.get(), isUniquelyOwned));

        for (const char* charKey = key.data(); charKey != key.data() + key.size(); ++charKey) {
            const uint8_t c = static_cast<const uint8_t>(*charKey);
            node = node->children[c];
            if (node == nullptr)
                return false;

            isUniquelyOwned = isUniquelyOwned && node.use_count() - 1 == 1;
            context.push_back(std::make_pair(node.get(), isUniquelyOwned));
        }

        size_t sizeOfRemovedNode = node->data->second.size();
        Node* last = context.back().first;
        isUniquelyOwned = context.back().second;
        context.pop_back();
        if (last->isLeaf()) {
            // If the node to be deleted is a leaf node, might need to prune the branch.
            uint8_t trieKey;
            while (!context.empty()) {
                trieKey = last->trieKey;
                last = context.back().first;
                isUniquelyOwned = context.back().second;
                context.pop_back();

                // If a node on the branch has data, stop pruning.
                if (last->data != boost::none)
                    break;

                // If a node has children other than the one leading to the to-be deleted node, stop
                // pruning.
                bool hasOtherChildren = false;
                for (auto iter = last->children.begin(); iter != last->children.end(); ++iter) {
                    if (*iter != nullptr && (*iter)->trieKey != trieKey) {
                        hasOtherChildren = true;
                        break;
                    }
                }

                if (hasOtherChildren)
                    break;
            }

            if (isUniquelyOwned) {
                // If this node is uniquely owned, simply set that child node to null and
                // "cut" off that branch of our tree
                last->children[trieKey] = nullptr;
            } else {
                // If its not uniquely owned, copy the branch so we can preserve it for the other
                // owner(s).
                std::shared_ptr<Node> child = std::make_shared<Node>(last->trieKey);
                auto lastIter = last->children.begin();
                for (auto iter = child->children.begin(); iter != child->children.end();
                     ++iter, ++lastIter) {
                    if (*lastIter != nullptr && (*lastIter)->trieKey == trieKey)
                        continue;

                    *iter = *lastIter;
                }

                std::shared_ptr<Node> node = child;
                while (!context.empty()) {
                    trieKey = last->trieKey;
                    last = context.back().first;
                    context.pop_back();
                    node = std::make_shared<Node>(last->trieKey);

                    auto lastIter = last->children.begin();
                    for (auto iter = node->children.begin(); iter != node->children.end();
                         ++iter, ++lastIter) {
                        *iter = *lastIter;
                    }

                    node->children[trieKey] = child;
                    child = node;
                }
                _root = node;
            }
        } else {
            // The to-be deleted node is an internal node, and therefore updating its data to be
            // boost::none will "delete" it
            _upsertWithCopyOnSharedNodes(key, boost::none);
        }

        _numElems--;
        _sizeElems -= sizeOfRemovedNode;
        return true;
    }

    // Returns a Store that has all changes from both 'this' and 'other' compared to base.
    // Throws merge_conflict_exception if there are merge conflicts.
    RadixStore merge3(const RadixStore& base, const RadixStore& other) const {
        RadixStore store;

        // Merges all differences between this and base, along with modifications from other.
        RadixStore::const_iterator iter = this->begin();
        while (iter != this->end()) {
            const value_type val = *iter;
            RadixStore::const_iterator baseIter = base.find(val.first);
            RadixStore::const_iterator otherIter = other.find(val.first);

            if (baseIter != base.end() && otherIter != other.end()) {
                if (val.second != baseIter->second && otherIter->second != baseIter->second) {
                    // Throws exception if there are conflicting modifications.
                    throw merge_conflict_exception();
                }

                if (val.second != baseIter->second) {
                    // Merges non-conflicting insertions from this.
                    store.insert(RadixStore::value_type(val));
                } else {
                    // Merges non-conflicting modifications from other or no modifications.
                    store.insert(RadixStore::value_type(*otherIter));
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
                store.insert(RadixStore::value_type(val));
            }
            iter++;
        }

        // Merges insertions and deletions from other.
        RadixStore::const_iterator other_iter = other.begin();
        for (; other_iter != other.end(); other_iter++) {
            const value_type otherVal = *other_iter;
            RadixStore::const_iterator baseIter = base.find(otherVal.first);
            RadixStore::const_iterator thisIter = this->find(otherVal.first);

            if (baseIter == base.end()) {
                // Merges insertions from other.
                store.insert(RadixStore::value_type(otherVal));
            } else if (thisIter == this->end() && otherVal.second != baseIter->second) {
                // Throws exception if modifications from this conflict with deletions from other.
                throw merge_conflict_exception();
            }
        }

        return store;
    }

    // iterators
    const_iterator begin() const noexcept {
        if (_numElems == 0) {
            return RadixStore::end();
        }

        auto node = _root;
        while (node->data == boost::none) {
            for (auto child : node->children) {
                if (child != nullptr) {
                    node = child;
                    break;
                }
            }
        }
        return RadixStore::const_iterator(_root, node.get());
    }

    const_reverse_iterator rbegin() const noexcept {
        if (_numElems == 0)
            return RadixStore::rend();

        auto node = _root;
        while (!node->isLeaf()) {
            for (auto iter = node->children.rbegin(); iter != node->children.rend(); ++iter) {
                if (*iter != nullptr) {
                    node = *iter;
                    break;
                }
            }
        }
        return RadixStore::const_reverse_iterator(_root, node.get());
    }

    const_iterator end() const noexcept {
        return RadixStore::const_iterator(_root);
    }

    const_reverse_iterator rend() const noexcept {
        return RadixStore::const_reverse_iterator(_root);
    }

    const_iterator find(const Key& key) const {
        RadixStore::const_iterator it = RadixStore::end();

        Node* node = _findNode(key);
        if (node == nullptr)
            return it;
        else
            return RadixStore::const_iterator(_root, node);
    }

    const_iterator lower_bound(const Key& key) const {
        Node* node = _root.get();
        std::vector<Node*> context;
        context.push_back(node);

        const char* charKey = key.data();
        uint8_t idx = '\0';

        // Traverse the path given the key to see if the node exists.
        for (; charKey != key.data() + key.size(); ++charKey) {
            idx = static_cast<uint8_t>(*charKey);
            if (node->children[idx] != nullptr) {
                node = node->children[idx].get();
                context.push_back(node);
            } else {
                break;
            }
        }

        // If the node existed, then can just return an iterator to that node.
        if (charKey == key.data() + key.size())
            return const_iterator(_root, node);

        // The node did not exist, so must find an node with the next largest key (if it exists).
        // Use the context stack to move up the tree and keep searching for the next node with data
        // if need be.
        while (!context.empty()) {
            node = context.back();
            context.pop_back();

            for (auto iter = idx + 1 + node->children.begin(); iter != node->children.end();
                 ++iter) {

                if (*iter != nullptr) {
                    // There exists a node with a key larger than the one given, traverse to this
                    // node which will be the left-most node in this sub-tree.
                    node = iter->get();
                    while (node->data == boost::none) {
                        for (auto iter = node->children.begin(); iter != node->children.end();
                             ++iter) {
                            if (*iter != nullptr) {
                                node = iter->get();
                                break;
                            }
                        }
                    }
                    return const_iterator(_root, node);
                }
            }
            idx = node->trieKey;
        }

        // If there was no node with a larger key than the one given, return end().
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

private:
    class Node {
        friend class RadixStore;

    public:
        Node(uint8_t key) : trieKey(key) {
            children.fill(nullptr);
        }

        bool isLeaf() {
            for (auto child : children) {
                if (child != nullptr)
                    return false;
            }
            return true;
        }

        uint8_t trieKey;
        boost::optional<value_type> data;
        std::array<std::shared_ptr<Node>, 256> children;
    };


    Node* _findNode(const Key& key) const {
        auto node = _root;
        for (const char* it = key.data(); it != key.data() + key.size(); ++it) {
            const uint8_t k = static_cast<const uint8_t>(*it);
            if (node->children[k] != nullptr)
                node = node->children[k];
            else
                return nullptr;
        }

        if (node->data == boost::none)
            return nullptr;

        return node.get();
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
        Key key, boost::optional<value_type> value) {

        auto node = _root;
        std::shared_ptr<Node> parent = nullptr;
        const char* keyString = key.data();
        size_t i = 0;

        // Follow the path in the tree as defined by the key string until a non-uniquely owned node.
        // This loop would exit at the root if the root itself was shared, or exit at the end in the
        // event the entire path was uniquely owned.
        for (; i < key.size(); i++) {

            // The current node in the traversal, if unique, will always have two pointers to it,
            // not one. This is because the parent node holds it as a child node, but now also we
            // have 'node' pointing to it. The loop will exit if the tree node is no longer uniquely
            // owned.
            if (node.use_count() > 2)
                break;

            uint8_t c = static_cast<uint8_t>(keyString[i]);

            if (node->children[c] != nullptr) {
                parent = node;
                node = node->children[c];
            } else {
                node->children[c] = std::make_shared<Node>(c);
                parent = node;
                node = node->children[c];
            }
        }

        std::shared_ptr<Node> old;

        if (i == 0) {
            // If the _root node is shared to begin with, copy the _root. This is necessary since
            // '_root' is a member variable and must be updated if changed, unlike other inner nodes
            // of the tree.
            old = _root;
            _root = std::make_shared<Node>('\0');
            node = _root;

        } else if (i < key.size()) {
            // If there is a shared node in the middle of the tree, backtrack and create a new node
            // that is singly owned by this tree. It is necessary to copy all following nodes as
            // well.
            old = node;
            uint8_t c = static_cast<uint8_t>(keyString[i - 1]);
            parent->children[c] = std::make_shared<Node>(c);
            node = parent->children[c];

        } else if (i >= key.size()) {
            // If all nodes prior to the last node in the traversal are uniquely owned, then set
            // 'old' to nullptr to prevent reassigning the node's children.
            old = nullptr;

            if (node.use_count() > 2) {
                // In the special case in which the to-be modified node (the last node in our
                // traversal) is itself the first non-uniquely owned node - copy it and reassign its
                // parents and children.
                old = node;
                uint8_t c = static_cast<uint8_t>(keyString[i - 1]);
                parent->children[c] = std::make_shared<Node>(c);
                node = parent->children[c];
            }
        }

        for (; i < key.size(); i++) {
            uint8_t c = static_cast<uint8_t>(keyString[i]);

            if (old != nullptr) {
                node->children = old->children;

                if (old->data != boost::none)
                    node->data.emplace(old->data->first, old->data->second);

                old = old->children[c];
            }

            node->children[c] = std::make_shared<Node>(c);
            node = node->children[c];
        }

        if (value != boost::none) {
            node->data.emplace(value->first, value->second);
        } else {
            node->data = boost::none;
        }

        // If 'old' isn't a nullptr, add the children since the modified node need not be a leaf in
        // the tree. Will only have to do this if the modified node was not uniquely owned, and a
        // copy was created.
        if (old != nullptr) {
            node->children = old->children;
        }

        const_iterator it(_root, node.get());
        return std::pair<const_iterator, bool>(it, true);
    }

    /**
    * This function traverses the tree starting at the provided node using the provided the
    * key. It returns the stack which is used in tree traversals for both the forward and
    * reverse iterators. Since both iterator classes use this function, it is declared
    * statically under RadixStore.
    */
    static std::vector<Node*> _buildContext(Key key, Node* node) {
        std::vector<Node*> context;
        context.push_back(node);
        for (const char* it = key.data(); it != key.data() + key.size(); ++it) {
            uint8_t c = static_cast<uint8_t>(*it);
            node = node->children[c].get();
            context.push_back(node);
        }
        return context;
    }

    std::shared_ptr<Node> _root;
    size_type _numElems;
    size_type _sizeElems;
};

using StringStore = RadixStore<std::string, std::string>;
}  // namespace biggie
}  // namespace mongo
