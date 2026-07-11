// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/field_ref_set.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <iterator>
#include <string_view>

#include <boost/move/utility_core.hpp>

namespace mongo {

using std::string;
using std::vector;

namespace {

// For legacy purposes, we must handle empty fieldnames, which FieldRef clearly
// prohibits. It is preferrable to have FieldRef keep that constraint and relax it here
// -- stricly in update code. The rationale is that, if we want to ban data with no
// field names, we must allow that data to be updated.
std::string_view safeFirstPart(const FieldRef* fieldRef) {
    if (fieldRef->numParts() == 0) {
        return std::string_view();
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

    std::string_view prefixStr = safeFirstPart(toCheck);
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
    std::string_view prefixStr = safeFirstPart(toInsert);
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
