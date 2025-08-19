/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef avro_NodeConcepts_hh__
#define avro_NodeConcepts_hh__

#include "Config.hh"

#include "Exception.hh"
#include <map>
#include <vector>

namespace avro {

///
/// The concept classes are used to simplify NodeImpl.  Since different types
/// of avro types carry different attributes, such as names, or field names for
/// record members.  Using the concept class of NoAttribute vs Attribute, the
/// NodeImpl object can enable/disable the attribute, but the code is the same
/// in either case.
///
/// Furthermore, attributes may have different types, for example, most
/// attributes are strings, but fixed types have a size attribute, which is
/// integer.
///
/// Since compound types are composed of other types, the leaf attribute
/// concepts extend a NodeImpl to include leaf nodes, and attributes for leaf
/// nodes, which are used to build parse trees.
///
///

namespace concepts {

template<typename Attribute>
struct NoAttribute {
    static const bool hasAttribute = false;

    size_t size() const {
        return 0;
    }

    void add(const Attribute & /* attr */) {
        // There must be an add function for the generic NodeImpl, but the
        // Node APIs ensure that it is never called, the throw here is
        // just in case
        throw Exception("This type does not have attribute");
    }

    const Attribute &get(size_t /* index */ = 0) const {
        // There must be an get function for the generic NodeImpl, but the
        // Node APIs ensure that it is never called, the throw here is
        // just in case
        throw Exception("This type does not have attribute");
    }

    Attribute &get(size_t /* index */ = 0) {
        // There must be an get function for the generic NodeImpl, but the
        // Node APIs ensure that it is never called, the throw here is
        // just in case
        throw Exception("This type does not have attribute");
    }
};

template<typename Attribute>
struct SingleAttribute {
    static const bool hasAttribute = true;

    SingleAttribute() : attr_() {}

    explicit SingleAttribute(const Attribute &a) : attr_(a) {}
    // copy constructing from another single attribute is allowed
    SingleAttribute(const SingleAttribute<Attribute> &rhs) : attr_(rhs.attr_) {}

    // copy constructing from a no attribute is allowed
    explicit SingleAttribute(const NoAttribute<Attribute> &rhs) : attr_() {}

    size_t size() const {
        return 1;
    }

    void add(const Attribute &attr) {
        attr_ = attr;
    }

    const Attribute &get(size_t index = 0) const {
        if (index != 0) {
            throw Exception("SingleAttribute has only 1 value");
        }
        return attr_;
    }

    Attribute &get(size_t index = 0) {
        if (index != 0) {
            throw Exception("SingleAttribute has only 1 value");
        }
        return attr_;
    }

private:
    template<typename T>
    friend struct MultiAttribute;
    Attribute attr_;
};

template<typename Attribute>
struct MultiAttribute {
    static const bool hasAttribute = true;

    MultiAttribute() = default;

    // copy constructing from another single attribute is allowed, it
    // pushes the attribute
    explicit MultiAttribute(const SingleAttribute<Attribute> &rhs) {
        // since map is the only type that does this we know it's
        // final size will be two, so reserve
        attrs_.reserve(2);
        attrs_.push_back(rhs.attr_);
    }

    MultiAttribute(const MultiAttribute<Attribute> &rhs) : attrs_(rhs.attrs_) {}

    explicit MultiAttribute(const NoAttribute<Attribute> &rhs) {}

    size_t size() const {
        return attrs_.size();
    }

    void add(const Attribute &attr) {
        attrs_.push_back(attr);
    }

    const Attribute &get(size_t index = 0) const {
        return attrs_.at(index);
    }

    Attribute &get(size_t index) {
        return attrs_.at(index);
    }

private:
    std::vector<Attribute> attrs_;
};

template<typename T>
struct NameIndexConcept {

    bool lookup(const std::string &, size_t &) const {
        throw Exception("Name index does not exist");
    }

    bool add(const ::std::string &, size_t) {
        throw Exception("Name index does not exist");
    }
};

template<>
struct NameIndexConcept<MultiAttribute<std::string>> {
    using IndexMap = std::map<std::string, size_t>;

    bool lookup(const std::string &name, size_t &index) const {
        auto iter = map_.find(name);
        if (iter == map_.end()) {
            return false;
        }
        index = iter->second;
        return true;
    }

    bool add(const ::std::string &name, size_t index) {
        bool added = false;
        auto lb = map_.lower_bound(name);
        if (lb == map_.end() || map_.key_comp()(name, lb->first)) {
            map_.insert(lb, IndexMap::value_type(name, index));
            added = true;
        }
        return added;
    }

private:
    IndexMap map_;
};

} // namespace concepts
} // namespace avro

#endif
