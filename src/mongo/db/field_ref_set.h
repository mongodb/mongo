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

#include <set>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/base/status.h"
#include "mongo/db/field_ref.h"

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
    MONGO_DISALLOW_COPYING(FieldRefSet);

    struct FieldRefPtrLessThan {
        bool operator()(const FieldRef* lhs, const FieldRef* rhs) const;
    };

    typedef std::set<const FieldRef*, FieldRefPtrLessThan> FieldSet;

public:
    typedef FieldSet::iterator iterator;
    typedef FieldSet::const_iterator const_iterator;

    FieldRefSet();

    FieldRefSet(const std::vector<FieldRef*>& paths);

    /** Returns 'true' if the set is empty */
    bool empty() const {
        return _fieldSet.empty();
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
     * Returns true if the field 'toInsert' can be added in the set without
     * conflicts. Otherwise returns false and fill in '*conflict' with the field 'toInsert'
     * clashed with.
     *
     * There is no ownership transfer of 'toInsert'. The caller is responsible for
     * maintaining it alive for as long as the FieldRefSet is so. By the same token
     * 'conflict' can only be referred to while the FieldRefSet can.
     */
    bool insert(const FieldRef* toInsert, const FieldRef** conflict);

    /**
     * Fills the set with the supplied FieldRef*s
     *
     * Note that *no* conflict resolution occurs here.
     */
    void fillFrom(const std::vector<FieldRef*>& fields);

    /**
     * Replace any existing conflicting FieldRef with the shortest (closest to root) one
     */
    void keepShortest(const FieldRef* toInsert);

    /**
     * Find all inserted fields which conflict with the FieldRef 'toCheck' by the semantics
     * of 'insert', and add those fields to the 'conflicts' set.
     *
     * Return true if conflicts were found.
     */
    bool findConflicts(const FieldRef* toCheck, FieldRefSet* conflicts) const;

    void clear() {
        _fieldSet.clear();
    }

    /**
     * A debug/log-able string
     */
    const std::string toString() const;

private:
    // A set of field_ref pointers, none of which is owned here.
    FieldSet _fieldSet;
};

}  // namespace mongo
