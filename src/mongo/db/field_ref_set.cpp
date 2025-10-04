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

#include "mongo/db/field_ref_set.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <iterator>

#include <boost/move/utility_core.hpp>

namespace mongo {

using std::string;
using std::vector;

namespace {

// For legacy purposes, we must handle empty fieldnames, which FieldRef clearly
// prohibits. It is preferrable to have FieldRef keep that constraint and relax it here
// -- stricly in update code. The rationale is that, if we want to ban data with no
// field names, we must allow that data to be updated.
StringData safeFirstPart(const FieldRef* fieldRef) {
    if (fieldRef->numParts() == 0) {
        return StringData();
    } else {
        return fieldRef->getPart(0);
    }
}


/**
 * Helper function to check if path conflicts are all prefixes.
 */
Status checkPathIsPrefixOf(const FieldRef& path, const FieldRef& conflictingPath) {
    // Conflicts are always prefixes (or equal to) the path, or vice versa
    if (path.numParts() > conflictingPath.numParts()) {
        string errMsg = str::stream()
            << "field at '" << conflictingPath.dottedField()
            << "' must be exactly specified, field at sub-path '" << path.dottedField() << "'found";
        return Status(ErrorCodes::NotExactValueField, errMsg);
    }

    return Status::OK();
}

}  // namespace

bool FieldRefSet::FieldRefPtrLessThan::operator()(const FieldRef* l, const FieldRef* r) const {
    return *l < *r;
}

FieldRefSet::FieldRefSet() {}

FieldRefSet::FieldRefSet(const std::vector<std::unique_ptr<FieldRef>>& paths) {
    fillFrom(paths);
}

FieldRefSet::FieldRefSet(const vector<const FieldRef*>& paths) {
    _fieldSet.insert(paths.begin(), paths.end());
}

FieldRefSet::FieldRefSet(const vector<FieldRef*>& paths) {
    fillFrom(paths);
}

StatusWith<bool> FieldRefSet::checkForConflictsAndPrefix(const FieldRef* toCheck) const {
    bool foundConflict = false;

    // If the set is empty, there is no work to do.
    if (_fieldSet.empty())
        return foundConflict;

    StringData prefixStr = safeFirstPart(toCheck);
    FieldRef prefixField(prefixStr);

    iterator it = _fieldSet.lower_bound(&prefixField);
    // Now, iterate over all the present fields in the set that have the same prefix.

    while (it != _fieldSet.end() && safeFirstPart(*it) == prefixStr) {
        size_t common = (*it)->commonPrefixSize(*toCheck);
        if ((*it)->numParts() == common || toCheck->numParts() == common) {
            if (auto status = checkPathIsPrefixOf(*toCheck, **it); !status.isOK()) {
                return status;
            }
            foundConflict = true;
        }
        ++it;
    }

    return foundConflict;
}

void FieldRefSet::keepShortest(const FieldRef* toInsert) {
    const FieldRef* conflict;
    if (!insert(toInsert, &conflict) && (toInsert->numParts() < (conflict->numParts()))) {
        _fieldSet.erase(conflict);
        keepShortest(toInsert);
    }
}

void FieldRefSet::fillFrom(const std::vector<FieldRef*>& fields) {
    dassert(_fieldSet.empty());
    _fieldSet.insert(fields.begin(), fields.end());
}

void FieldRefSet::fillFrom(const std::vector<std::unique_ptr<FieldRef>>& fields) {
    dassert(_fieldSet.empty());
    std::transform(fields.begin(),
                   fields.end(),
                   std::inserter(_fieldSet, _fieldSet.begin()),
                   [](const auto& field) { return field.get(); });
}

bool FieldRefSet::insertNoConflict(const FieldRef* toInsert) {
    const FieldRef* conflict;
    return insert(toInsert, &conflict);
}

bool FieldRefSet::insert(const FieldRef* toInsert, const FieldRef** conflict) {
    // We can determine if two fields conflict by checking their common prefix.
    //
    // If each field is exactly of the size of the common prefix, this means the fields are
    // the same. If one of the fields is greater than the common prefix and the other
    // isn't, the latter is a prefix of the former. And vice-versa.
    //
    // Example:
    //
    // inserted >      |    a          a.c
    // exiting  v      |   (0)        (+1)
    // ----------------|------------------------
    //      a (0)      |  equal      prefix <
    //      a.b (+1)   | prefix ^      *
    //
    // * Disjoint sub-trees

    // At each insertion, we only need to bother checking the fields in the set that have
    // at least some common prefix with the 'toInsert' field.
    StringData prefixStr = safeFirstPart(toInsert);
    FieldRef prefixField(prefixStr);
    iterator it = _fieldSet.lower_bound(&prefixField);

    // Now, iterate over all the present fields in the set that have the same prefix.
    while (it != _fieldSet.end() && safeFirstPart(*it) == prefixStr) {
        size_t common = (*it)->commonPrefixSize(*toInsert);
        if ((*it)->numParts() == common || toInsert->numParts() == common) {
            *conflict = *it;
            return false;
        }
        ++it;
    }

    _fieldSet.insert(it, toInsert);
    *conflict = nullptr;
    return true;
}

std::string FieldRefSet::toString() const {
    str::stream ss;
    ss << "{";
    const auto last = _fieldSet.rbegin();
    for (auto path : _fieldSet) {
        ss << path->dottedField();
        if (path != *last)
            ss << ", ";
    }
    ss << "}";
    return ss;
}

}  // namespace mongo
