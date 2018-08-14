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
#include <cstring>
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
            // Pop back once more to get access to its parent node. The parent node will enable
            // traversal through the neighboring nodes, and if there are none, the iterator will
            // move up the tree to continue searching for the next node with data.
            Node* node = context.back();
            context.pop_back();

            // In case there is no next node, set _current to be 'nullptr' which will mark the end
            // of the traversal.
            _current = nullptr;
            while (!context.empty()) {
                uint8_t oldKey = node->trieKey.front();
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
                oldKey = node->trieKey.front();
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
    RadixStore() {
        _root = std::make_shared<RadixStore::Node>();
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
        _root = std::make_shared<Node>();
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

        const char* charKey = key.data();
        size_t depth = 0;
        while (depth < key.size()) {
            uint8_t c = static_cast<uint8_t>(charKey[depth]);
            node = node->children[c];

            if (node == nullptr) {
                return 0;
            }

            // If the prefixes mismatch, this key cannot exist in the tree.
            size_t p = _comparePrefix(node->trieKey, charKey + depth, key.size() - depth);
            if (p != node->trieKey.size()) {
                return 0;
            }

            isUniquelyOwned = isUniquelyOwned && node.use_count() - 1 == 1;
            context.push_back(std::make_pair(node.get(), isUniquelyOwned));
            depth += node->trieKey.size();
        }

        size_t sizeOfRemovedNode = node->data->second.size();
        Node* deleted = context.back().first;
        context.pop_back();

        Node* last = context.back().first;
        isUniquelyOwned = context.back().second;
        context.pop_back();

        if (deleted->isLeaf()) {
            uint8_t firstChar = deleted->trieKey.front();
            if (isUniquelyOwned) {
                // If this node is uniquely owned, simply set that child node to null and
                // "cut" off that branch of our tree
                last->children[firstChar] = nullptr;
                _compressOnlyChild(last);
            } else {
                // If it's not uniquely owned, copy 'last' before deleting the node that
                // matches key.
                std::shared_ptr<Node> child = std::make_shared<Node>(*last);
                child->children[firstChar] = nullptr;

                // 'last' may only have one child, in which case we need to evaluate
                // whether or not this node is redundant.
                _compressOnlyChild(child.get());

                // Continue copying the rest of the branch so we can preserve it for the
                // other owner(s).
                std::shared_ptr<Node> node = child;
                while (!context.empty()) {
                    firstChar = last->trieKey.front();
                    last = context.back().first;
                    context.pop_back();

                    node = std::make_shared<Node>(*last);
                    node->children[firstChar] = child;
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
        return 1;
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
        // When we search a child array, always search to the right of 'idx' so that
        // when we go back up the tree we never search anything less than something
        // we already examined.
        uint8_t idx = '\0';
        size_t depth = 0;

        // Traverse the path given the key to see if the node exists.
        while (depth < key.size()) {
            idx = static_cast<uint8_t>(charKey[depth]);
            if (node->children[idx] == nullptr) {
                break;
            }

            node = node->children[idx].get();
            // We may eventually need to search this node's parent for larger children
            idx += 1;
            size_t mismatchIdx = _comparePrefix(node->trieKey, charKey + depth, key.size() - depth);

            // There is a prefix mismatch, so we don't need to traverse anymore
            if (mismatchIdx < node->trieKey.size()) {
                // Check if the current key in the tree is greater than the one we are looking
                // for since it can't be equal at this point. It can be greater in two ways:
                // It can be longer or it can have a larger character at the mismatch index.
                uint8_t mismatchChar = static_cast<uint8_t>(charKey[mismatchIdx + depth]);
                if (mismatchIdx == key.size() - depth ||
                    node->trieKey[mismatchIdx] > mismatchChar) {
                    // If the current key is greater and has a value it is the lower bound.
                    if (node->data != boost::none) {
                        return const_iterator(_root, node);
                    }

                    // If the current key has no value, place it in the context
                    // so that we can search its children.
                    context.push_back(node);
                    idx = '\0';
                } else {
                    // If the current key is less, we will need to go back up the
                    // tree and this node does not need to be pushed into the context.
                    idx = static_cast<uint8_t>(charKey[depth]) + 1;
                }
                break;
            }

            context.push_back(node);
            depth += node->trieKey.size();
        }

        if (depth == key.size() && node->data != boost::none) {
            // If the node exists, then we can just return an iterator to that node.
            return const_iterator(_root, node);
        } else if (depth == key.size()) {
            // The search key is an exact prefix, so we need to search all of this node's
            // children.
            idx = '\0';
        }

        // The node did not exist, so must find an node with the next largest key (if it exists).
        // Use the context stack to move up the tree and keep searching for the next node with data
        // if need be.
        while (!context.empty()) {
            node = context.back();
            context.pop_back();

            for (auto iter = idx + node->children.begin(); iter != node->children.end(); ++iter) {
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

            if (node->trieKey.empty()) {
                // We have searched the root. There's nothing left to search.
                return end();
            } else {
                idx = node->trieKey.front() + 1;
            }
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

    std::string to_string_for_test() {
        return _walkTree(_root.get(), 0);
    }

private:
    class Node {
        friend class RadixStore;

    public:
        Node() {
            children.fill(nullptr);
        }

        Node(std::vector<uint8_t> key) : trieKey(key) {
            children.fill(nullptr);
        }

        bool isLeaf() {
            for (auto child : children) {
                if (child != nullptr)
                    return false;
            }
            return true;
        }

        std::vector<uint8_t> trieKey;
        boost::optional<value_type> data;
        std::array<std::shared_ptr<Node>, 256> children;
    };

    /*
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

        for (uint8_t ch : node->trieKey) {
            ret.push_back(ch);
        }
        if (node->data != boost::none) {
            ret.push_back('*');
        }
        ret.push_back('\n');

        for (auto child : node->children) {
            if (child != nullptr) {
                ret.append(_walkTree(child.get(), depth + 1));
            }
        }
        return ret;
    }

    Node* _findNode(const Key& key) const {
        int depth = 0;
        uint8_t childFirstChar = static_cast<uint8_t>(key.front());
        auto node = _root->children[childFirstChar];

        const char* charKey = key.data();

        while (node != nullptr) {

            size_t mismatchIdx = _comparePrefix(node->trieKey, charKey + depth, key.size() - depth);
            if (mismatchIdx != node->trieKey.size()) {
                return nullptr;
            } else if (mismatchIdx == key.size() - depth && node->data != boost::none) {
                return node.get();
            }

            depth += node->trieKey.size();

            childFirstChar = static_cast<uint8_t>(charKey[depth]);
            node = node->children[childFirstChar];
        }

        return nullptr;
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

        const char* charKey = key.data();
        int depth = 0;

        uint8_t childFirstChar = static_cast<uint8_t>(charKey[depth]);
        std::shared_ptr<Node> node = _root->children[childFirstChar];
        std::shared_ptr<Node> old = node;

        // Copy root if it is not uniquely owned.
        if (_root.use_count() > 1) {
            _root = std::make_shared<Node>(*_root.get());
        }

        std::shared_ptr<Node> prev = _root;
        while (node != nullptr) {
            // Copy node if it is not uniquely owned. A unique node will always have two pointers
            // to it. One for the parent node and one for the 'node' variable.
            // 'prev' should always be uniquely owned and so we should be able to modify it.
            if (node.use_count() > 2) {
                node = std::make_shared<Node>(*old.get());
                prev->children[old->trieKey.front()] = node;
            }

            // 'node' is uniquely owned at this point, so we are free to modify it.
            // Get the index at which node->trieKey and the new key differ.
            size_t mismatchIdx = _comparePrefix(node->trieKey, charKey + depth, key.size() - depth);

            // The keys mismatch, so we need to split this node.
            if (mismatchIdx != node->trieKey.size()) {
                // Make a new node with whatever prefix is shared between node->trieKey
                // and the new key. This will replace the current node in the tree.
                std::vector<uint8_t> newKey = _makeKey(node->trieKey, 0, mismatchIdx);
                auto newNode = _addChild(prev, newKey, boost::none);

                depth += mismatchIdx;
                const_iterator it(_root, newNode.get());
                if (key.size() - depth != 0) {
                    // Make a child with whatever is left of the new key.
                    newKey = _makeKey(charKey + depth, key.size() - depth);
                    auto newChild = _addChild(newNode, newKey, value);
                    it = const_iterator(_root, newChild.get());
                } else {
                    // The new key is a prefix of an existing key, and has its own node,
                    // so we don't need to add any new nodes.
                    newNode->data.emplace(value->first, value->second);
                }

                // Change the current node's trieKey and make a child of the new node.
                newKey = _makeKey(node->trieKey, mismatchIdx, node->trieKey.size() - mismatchIdx);
                newNode->children[newKey.front()] = node;
                node->trieKey = newKey;

                return std::pair<const_iterator, bool>(it, true);
            } else if (mismatchIdx == key.size() - depth) {
                // Update an internal node
                if (value == boost::none) {
                    node->data = boost::none;
                    _compressOnlyChild(node.get());
                } else {
                    node->data.emplace(value->first, value->second);
                }
                const_iterator it(_root, node.get());
                return std::pair<const_iterator, bool>(it, true);
            }

            depth += node->trieKey.size();
            childFirstChar = static_cast<const uint8_t>(charKey[depth]);
            prev = node;
            node = node->children[childFirstChar];

            if (old != nullptr) {
                old = old->children[childFirstChar];
            }
        }

        // Add a completely new child to a node. The new key at this depth does not
        // share a prefix with any existing keys.
        std::vector<uint8_t> newKey = _makeKey(charKey + depth, key.size() - depth);
        auto newNode = _addChild(prev, newKey, value);
        const_iterator it(_root, newNode.get());
        return std::pair<const_iterator, bool>(it, true);
    }

    /*
     * Return a uint8_t vector with the first 'count' characters of
     * 'old'.
     */
    std::vector<uint8_t> _makeKey(const char* old, size_t count) {
        std::vector<uint8_t> key;
        for (size_t i = 0; i < count; ++i) {
            uint8_t c = static_cast<uint8_t>(old[i]);
            key.push_back(c);
        }
        return key;
    }

    /*
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
     * Add a child with trieKey 'key' and value 'value' to 'node'.
     */
    std::shared_ptr<Node> _addChild(std::shared_ptr<Node> node,
                                    std::vector<uint8_t> key,
                                    boost::optional<value_type> value) {
        std::shared_ptr<Node> newNode = std::make_shared<Node>(key);
        if (value != boost::none) {
            newNode->data.emplace(value->first, value->second);
        }
        node->children[key.front()] = newNode;
        return newNode;
    }

    /**
    * This function traverses the tree starting at the provided node using the provided the
    * key. It returns the stack which is used in tree traversals for both the forward and
    * reverse iterators. Since both iterator classes use this function, it is declared
    * statically under RadixStore.
    *
    * This assumes that the key is present in the tree.
    *
    */
    static std::vector<Node*> _buildContext(Key key, Node* node) {
        std::vector<Node*> context;
        context.push_back(node);

        const char* charKey = key.data();
        size_t depth = 0;

        while (depth < key.size()) {
            uint8_t c = static_cast<uint8_t>(charKey[depth]);
            node = node->children[c].get();
            context.push_back(node);
            depth = depth + node->trieKey.size();
        }
        return context;
    }

    /*
     * Return the index at which 'key1' and 'key2' differ.
     *
     * This function will interpret the bytes in 'key2' as unsigned values.
     *
     */
    size_t _comparePrefix(std::vector<uint8_t> key1, const char* key2, size_t len2) const {
        size_t smaller = std::min(key1.size(), len2);

        size_t i = 0;
        for (; i < smaller; ++i) {
            uint8_t c = static_cast<uint8_t>(key2[i]);
            if (key1[i] != c) {
                return i;
            }
        }
        return i;
    }

    /*
     * Compresses a child node into its parent if necessary.
     *
     * This is required when an erase results in a node with no value
     * and only one child.
     *
     */
    void _compressOnlyChild(Node* node) {
        // Don't compress if this node has an actual value associated with it
        // or is the root.
        if (node->data != boost::none || node->trieKey.empty()) {
            return;
        }

        // Determine if this node has only one child.
        std::shared_ptr<Node> onlyChild = nullptr;
        for (size_t i = 0; i < node->children.size(); ++i) {
            if (node->children[i] != nullptr) {
                if (onlyChild != nullptr) {
                    return;
                }
                onlyChild = node->children[i];
            }
        }

        // Append the child's key onto the parent.
        for (char item : onlyChild->trieKey) {
            node->trieKey.push_back(item);
        }

        if (onlyChild->data != boost::none) {
            node->data.emplace(onlyChild->data->first, onlyChild->data->second);
        }
        node->children = onlyChild->children;
    }

    std::shared_ptr<Node> _root;
    size_type _numElems;
    size_type _sizeElems;
};

using StringStore = RadixStore<std::string, std::string>;
}  // namespace biggie
}  // namespace mongo
