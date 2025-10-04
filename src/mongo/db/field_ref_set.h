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

#include <boost/container/small_vector.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/db/field_ref.h"

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>

namespace mongo {

/**
 * A FieldRefSet holds a number of unique FieldRefs - a set of dotted paths into a document.
 *
 * The FieldRefSet provides helpful functions for efficiently finding conflicts between field
 * ref paths - field ref paths conflict if they are equal to each other or if one is a prefix.
 * To maintain a FieldRefSet of non-conflicting paths, always use the insert method which
 * returns conflicting FieldRefs.
 *
 * FieldRefSets do not own the FieldRef paths they contain.
 */
class FieldRefSet {
    FieldRefSet(const FieldRefSet&) = delete;
    FieldRefSet& operator=(const FieldRefSet&) = delete;

    struct FieldRefPtrLessThan {
        bool operator()(const FieldRef* lhs, const FieldRef* rhs) const;
    };

    typedef std::set<const FieldRef*, FieldRefPtrLessThan> SetType;

public:
    using iterator = SetType::iterator;
    using const_iterator = SetType::const_iterator;

    FieldRefSet();

    FieldRefSet(const std::vector<std::unique_ptr<FieldRef>>& paths);
    FieldRefSet(const std::vector<const FieldRef*>& paths);
    FieldRefSet(const std::vector<FieldRef*>& paths);

    /** Returns 'true' if the set is empty */
    bool empty() const {
        return _fieldSet.empty();
    }

    size_t size() const {
        return _fieldSet.size();
    }

    inline const_iterator begin() const {
        return _fieldSet.begin();
    }

    inline const_iterator end() const {
        return _fieldSet.end();
    }

    /**
     * Returns true if the path does not already exist in the set, false otherwise.
     *
     * Note that *no* conflict resolution occurs - any path can be inserted into a set.
     */
    inline bool insert(const FieldRef* path) {
        return _fieldSet.insert(path).second;
    }

    /**
     * Returns true if the field 'toInsert' was added to the set without conflicts.
     *
     * Otherwise, returns false and fills '*conflict' with the field 'toInsert' clashed with.
     *
     * There is no ownership transfer of 'toInsert'. The caller is responsible for
     * maintaining it alive for as long as the FieldRefSet is so. By the same token
     * 'conflict' can only be referred to while the FieldRefSet can.
     */
    bool insert(const FieldRef* toInsert, const FieldRef** conflict);

    /**
     * Returns true if the field 'toInsert' was added to the set without conflicts.
     */
    bool insertNoConflict(const FieldRef* toInsert);

    /**
     * Fills the set with the supplied FieldRef pointers.
     *
     * Note that *no* conflict resolution occurs here.
     */
    void fillFrom(const std::vector<FieldRef*>& fields);

    /**
     * Fills the set with the supplied FieldRefs. Does not take ownership of the managed pointers.
     *
     * Note that *no* conflict resolution occurs here.
     */
    void fillFrom(const std::vector<std::unique_ptr<FieldRef>>& fields);

    /**
     * Replace any existing conflicting FieldRef with the shortest (closest to root) one.
     */
    void keepShortest(const FieldRef* toInsert);

    /**
     * Find all inserted fields which conflict with the FieldRef 'toCheck' by the semantics
     * of 'insert', and add those fields to the 'conflicts' set.
     *
     * Return true if conflicts were found.
     */
    StatusWith<bool> checkForConflictsAndPrefix(const FieldRef* toCheck) const;

    void clear() {
        _fieldSet.clear();
    }

    void erase(const FieldRef* item) {
        _fieldSet.erase(item);
    }

    /**
     * A debug/log-able string
     */
    std::string toString() const;

private:
    // A set of field_ref pointers, none of which is owned here.
    SetType _fieldSet;
};

/**
 * A wrapper class for FieldRefSet which owns the storage of the underlying FieldRef objects.
 */
class FieldRefSetWithStorage {
public:
    /**
     * Inserts the given FieldRef into the set. In the case of a conflict with an existing element,
     * only the shortest path is kept in the set.
     */
    void keepShortest(const FieldRef& toInsert) {
        const FieldRef* inserted = &(*_ownedFieldRefs.insert(toInsert).first);
        _fieldRefSet.keepShortest(inserted);
    }

    std::vector<std::string> serialize() const {
        std::vector<std::string> ret;
        for (const auto fieldRef : _fieldRefSet) {
            ret.push_back(std::string{fieldRef->dottedField()});
        }
        return ret;
    }

    bool empty() const {
        return _fieldRefSet.empty();
    }

    void clear() {
        _ownedFieldRefs.clear();
        _fieldRefSet.clear();
    }

    std::string toString() const {
        return _fieldRefSet.toString();
    }

private:
    // Holds the storage for FieldRef's inserted into the set. This may become out of sync with
    // '_fieldRefSet' since we don't attempt to remove conflicts from the backing set, which can
    // leave '_ownedFieldRefs' holding storage for a superset of the field refs that are actually
    // contained in '_fieldRefSet'.
    std::set<FieldRef> _ownedFieldRefs;
    FieldRefSet _fieldRefSet;
};

}  // namespace mongo
