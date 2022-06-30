/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>

#include "mongo/base/status.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/db/cst/c_node.h"
#include "mongo/db/cst/c_node_validation.h"
#include "mongo/db/cst/path.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/variable_validation.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/stdx/variant.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo::c_node_validation {
using namespace std::string_literals;
namespace {

template <typename Iter, typename EndFun>
StatusWith<IsInclusion> processAdditionalFieldsInclusionAssumed(const Iter& iter,
                                                                const EndFun& isEnd);
template <typename Iter, typename EndFun>
StatusWith<IsInclusion> processAdditionalFieldsExclusionAssumed(const Iter& iter,
                                                                const EndFun& isEnd);

auto isInclusionField(const CNode& project) {
    if (auto type = project.projectionType())
        switch (*type) {
            case ProjectionType::inclusion:
                // This is an inclusion Key.
                return true;
            case ProjectionType::exclusion:
                // This is an exclusion Key.
                return false;
            default:
                MONGO_UNREACHABLE;
        }
    else
        // This is an arbitrary expression to produce a computed field (this counts as inclusion).
        return true;
}

template <typename Iter, typename EndFun>
StatusWith<IsInclusion> processAdditionalFieldsInclusionConfirmed(const Iter& iter,
                                                                  const EndFun& isEnd) {
    if (!isEnd(iter)) {
        if (CNode::fieldnameIsId(iter->first)) {
            return processAdditionalFieldsInclusionConfirmed(std::next(iter), isEnd);
        } else {
            if (isInclusionField(iter->second))
                return processAdditionalFieldsInclusionConfirmed(std::next(iter), isEnd);
            else
                return Status{ErrorCodes::FailedToParse,
                              "project containing inclusion and/or computed fields must "
                              "contain no exclusion fields"};
        }
    } else {
        return IsInclusion::yes;
    }
}

template <typename Iter, typename EndFun>
StatusWith<IsInclusion> processAdditionalFieldsExclusionConfirmed(const Iter& iter,
                                                                  const EndFun& isEnd) {
    if (!isEnd(iter)) {
        if (CNode::fieldnameIsId(iter->first)) {
            return processAdditionalFieldsExclusionConfirmed(std::next(iter), isEnd);
        } else {
            if (isInclusionField(iter->second))
                return Status{ErrorCodes::FailedToParse,
                              "project containing exclusion fields must contain no "
                              "inclusion and/or computed fields"};
            else
                return processAdditionalFieldsExclusionConfirmed(std::next(iter), isEnd);
        }
    } else {
        return IsInclusion::no;
    }
}

template <typename Iter, typename EndFun>
StatusWith<IsInclusion> processAdditionalFieldsWhenAssuming(const Iter& iter, const EndFun& isEnd) {
    if (CNode::fieldnameIsId(iter->first)) {
        if (isInclusionField(iter->second))
            return processAdditionalFieldsInclusionAssumed(std::next(iter), isEnd);
        else
            return processAdditionalFieldsExclusionAssumed(std::next(iter), isEnd);
    } else {
        if (isInclusionField(iter->second))
            return processAdditionalFieldsInclusionConfirmed(std::next(iter), isEnd);
        else
            return processAdditionalFieldsExclusionConfirmed(std::next(iter), isEnd);
    }
}

template <typename Iter, typename EndFun>
StatusWith<IsInclusion> processAdditionalFieldsInclusionAssumed(const Iter& iter,
                                                                const EndFun& isEnd) {
    if (!isEnd(iter))
        return processAdditionalFieldsWhenAssuming(iter, isEnd);
    else
        return IsInclusion::yes;
}

template <typename Iter, typename EndFun>
StatusWith<IsInclusion> processAdditionalFieldsExclusionAssumed(const Iter& iter,
                                                                const EndFun& isEnd) {
    if (!isEnd(iter))
        return processAdditionalFieldsWhenAssuming(iter, isEnd);
    else
        return IsInclusion::no;
}

Status validatePathComponent(const std::string& component) {
    if (component.empty())
        return Status{ErrorCodes::FailedToParse, "field path is empty"};
    if (std::string::npos != component.find('\0'))
        return Status{ErrorCodes::FailedToParse, "field path contains null byte"};
    return Status::OK();
}

auto validateNotPrefix(const std::vector<StringData>& potentialPrefixOne,
                       const std::vector<StringData>& potentialPrefixTwo) {
    // If all components examined are identical up to a point where one path is exhausted,
    // one path is a prefix of the other (or they're equal but this equality is already checked
    // by the set emplace operation).
    for (auto n = decltype(potentialPrefixOne.size()){0ull};
         n < std::min(potentialPrefixOne.size(), potentialPrefixTwo.size());
         ++n)
        if (potentialPrefixOne[n] != potentialPrefixTwo[n])
            return Status::OK();
    return Status{ErrorCodes::FailedToParse,
                  "paths appearing in project conflict because one is a prefix of the other: "s +
                      path::vectorToString(potentialPrefixOne) + " & " +
                      path::vectorToString(potentialPrefixTwo)};
}

/**
 * Validate a path by checking to make sure it was never seen by using set uniqueness. In addition
 * to checking that it is not a prefix of another path and no path is a prefix of it. This function
 * modifies seenPaths in order to keep track.
 */
auto validateNotRedundantOrPrefixConflicting(const std::vector<StringData>& currentPath,
                                             std::set<std::vector<StringData>>* const seenPaths) {
    // The set 'seenPaths' is lexicographically ordered and we check only the next and previous
    // elements for the prefix relationship. If a path is a prefix of another path, that path
    // must appear next in order based on the invariant that the set has no prefix relationships
    // before the most recent 'emplace()'. If another path is the prefix of the emplaced path,
    // it must appear directly previous in order since any sibling that could otherwise appear
    // previous would be also prefixed by the path that prefixes the emplaced path and violate
    // the invariant. Thus it sufficies to check only these two positions in the set after
    // emplacing to guarantee there are no prefix relationships in the entire set.
    if (auto&& [iter, notDuplicate] = seenPaths->emplace(currentPath); notDuplicate) {
        if (iter != seenPaths->begin())
            if (auto status = validateNotPrefix(currentPath, *std::prev(iter)); !status.isOK())
                return status;
        if (std::next(iter) != seenPaths->end())
            if (auto status = validateNotPrefix(currentPath, *std::next(iter)); !status.isOK())
                return status;
        return Status::OK();
    } else {
        return Status{ErrorCodes::FailedToParse,
                      "path appears more than once in project: "s +
                          path::vectorToString(currentPath)};
    }
}

Status addPathsFromTreeToSet(const CNode::ObjectChildren& children,
                             const std::vector<StringData>& previousPath,
                             std::set<std::vector<StringData>>* const seenPaths) {
    for (auto&& child : children) {
        // Add all path components which make up the fieldname of the current child to
        // currentPath. FieldnamePath may introduce more than one if it originated from syntax
        // like '{"a.b": 1}'.
        auto currentPath = previousPath;
        if (auto&& fieldname = stdx::get_if<FieldnamePath>(&child.first))
            for (auto&& component :
                 stdx::visit([](auto&& fn) -> auto&& { return fn.components; }, *fieldname))
                currentPath.emplace_back(component);
        // Or add a translaiton of _id if we have a key for that.
        else
            currentPath.emplace_back("_id"_sd);

        // Ensure that the tree is constructed correctly. Confirm anything that's not a
        // FieldnamePath is actually _id.
        dassert(stdx::holds_alternative<FieldnamePath>(child.first) ||
                (stdx::holds_alternative<KeyFieldname>(child.first) &&
                 stdx::get<KeyFieldname>(child.first) == KeyFieldname::id));

        if (auto status = stdx::visit(
                OverloadedVisitor{
                    [&](const CompoundInclusionKey& compoundKey) {
                        // In this context we have a compound inclusion key to descend into.
                        return addPathsFromTreeToSet(
                            std::as_const(compoundKey.obj->objectChildren()),
                            currentPath,
                            seenPaths);
                    },
                    [&](const CompoundExclusionKey& compoundKey) {
                        // In this context we have a compound exclusion key to descend into.
                        return addPathsFromTreeToSet(
                            std::as_const(compoundKey.obj->objectChildren()),
                            currentPath,
                            seenPaths);
                    },
                    [&](const CNode::ObjectChildren& objectChildren) {
                        if (stdx::holds_alternative<FieldnamePath>(objectChildren[0].first))
                            // In this context we have a project path object to recurse over.
                            return addPathsFromTreeToSet(objectChildren, currentPath, seenPaths);
                        else
                            // We have a leaf from the point of view of computing paths.
                            return validateNotRedundantOrPrefixConflicting(currentPath, seenPaths);
                    },
                    [&](auto&&) {
                        // We have a leaf from the point of view of computing paths.
                        return validateNotRedundantOrPrefixConflicting(currentPath, seenPaths);
                    }},
                child.second.payload);
            !status.isOK())
            // If a redundant path is found, return early and report this.
            return status;
    }
    return Status::OK();
}

template <typename T>
Status validateNumericType(T num) {
    auto valueAsInt = BSON("" << num).firstElement().parseIntegerElementToInt();
    if (!valueAsInt.isOK() || valueAsInt.getValue() == 0 ||
        !isValidBSONType(valueAsInt.getValue())) {
        if constexpr (std::is_same_v<std::decay_t<T>, UserDecimal>) {
            return Status{ErrorCodes::FailedToParse,
                          str::stream() << "invalid numerical type code: " << num.toString()
                                        << " provided as argument"};
        } else {
            return Status{ErrorCodes::FailedToParse,
                          str::stream()
                              << "invalid numerical type code: " << num << " provided as argument"};
        }
    }
    return Status::OK();
}

Status validateSingleType(const CNode& element) {
    return stdx::visit(
        OverloadedVisitor{
            [&](const UserDouble& dbl) { return validateNumericType(dbl); },
            [&](const UserInt& num) { return validateNumericType(num); },
            [&](const UserLong& lng) { return validateNumericType(lng); },
            [&](const UserDecimal& dc) { return validateNumericType(dc); },
            [&](const UserString& st) {
                if (st == MatcherTypeSet::kMatchesAllNumbersAlias) {
                    return Status::OK();
                }
                auto optValue = findBSONTypeAlias(st);
                if (!optValue) {
                    // The string "missing" can be returned from the $type agg expression, but is
                    // not valid for use in the $type match expression predicate. Return a special
                    // error message for this case.
                    if (st == StringData{typeName(BSONType::EOO)}) {
                        return Status{
                            ErrorCodes::FailedToParse,
                            "unknown type name alias 'missing' (to query for "
                            "non-existence of a field, use {$exists:false}) provided as argument"};
                    }
                    return Status{ErrorCodes::FailedToParse,
                                  str::stream() << "unknown type name alias: '" << st
                                                << "' provided as argument"};
                }
                return Status::OK();
            },
            [&](auto &&) -> Status { MONGO_UNREACHABLE; }},
        element.payload);
}
}  // namespace

StatusWith<IsInclusion> validateProjectionAsInclusionOrExclusion(const CNode& projects) {
    return processAdditionalFieldsInclusionAssumed(
        projects.objectChildren().cbegin(),
        [&](auto&& iter) { return iter == projects.objectChildren().cend(); });
}

Status validateNoConflictingPathsInProjectFields(const CNode& projects) {
    // A collection of all paths previously seen. Purposefully ordered. Vector orders
    // lexicographically.
    auto seenPaths = std::set<std::vector<StringData>>{};
    return addPathsFromTreeToSet(projects.objectChildren(), std::vector<StringData>{}, &seenPaths);
}

Status validateAggregationPath(const std::vector<std::string>& components) {
    if (components.size() > BSONDepth::getMaxAllowableDepth())
        return Status{ErrorCodes::FailedToParse,
                      "aggregation field path has too many dot-seperated parts"};
    if (components[0][0] == '$')
        return Status{ErrorCodes::FailedToParse,
                      "aggregation field path begins with dollar character"};
    for (auto n = 0ull; n < components.size(); ++n)
        if (auto status = validatePathComponent(components[n]); !status.isOK())
            return status.withReason("component " + std::to_string(n) + " of aggregation "s +
                                     status.reason());
    return Status::OK();
}

Status validateVariableNameAndPathSuffix(const std::vector<std::string>& nameAndPathComponents) {
    try {
        variableValidation::validateNameForUserRead(nameAndPathComponents[0]);
    } catch (AssertionException& ae) {
        return Status{ae.code(), ae.reason()};
    }
    if (nameAndPathComponents.size() > BSONDepth::getMaxAllowableDepth())
        return Status{ErrorCodes::FailedToParse,
                      "aggregation variable field path has too many dot-seperated parts"};
    // Skip the variable prefix since it's already been checked.
    for (auto n = 1ull; n < nameAndPathComponents.size(); ++n)
        if (auto status = validatePathComponent(nameAndPathComponents[n]); !status.isOK())
            return status.withReason("component " + std::to_string(n) +
                                     " of aggregation variable "s + status.reason());
    return Status::OK();
}

StatusWith<IsPositional> validateProjectionPathAsNormalOrPositional(
    const std::vector<std::string>& components) {
    if (components.size() > BSONDepth::getMaxAllowableDepth())
        return Status{ErrorCodes::FailedToParse,
                      "projection field path has too many dot-seperated parts"};
    auto isPositional =
        components[components.size() - 1] == "$" ? IsPositional::yes : IsPositional::no;
    if (isPositional == IsPositional::no && components[0][0] == '$')
        return Status{ErrorCodes::FailedToParse,
                      "projection field path begins with dollar character"};
    for (auto n = 0ull; n < components.size() - (isPositional == IsPositional::yes ? 1 : 0); ++n)
        if (auto status = validatePathComponent(components[n]); !status.isOK())
            return status.withReason("component " + std::to_string(n) + " of projection "s +
                                     status.reason());
    return isPositional;
}

Status validateSortPath(const std::vector<std::string>& pathComponents) {
    try {
        for (auto&& component : pathComponents) {
            FieldPath::uassertValidFieldName(component);
        }
    } catch (AssertionException& ae) {
        return Status{ae.code(), ae.reason()};
    }
    return Status::OK();
}

Status validateTypeOperatorArgument(const CNode& types) {
    // If the CNode is an array, we need to validate all of the types within it.
    if (auto&& children = stdx::get_if<CNode::ArrayChildren>(&types.payload)) {
        for (auto&& child : (*children)) {
            if (auto status = validateSingleType(child); !status.isOK()) {
                return status;
            }
        }
        return Status::OK();
    }
    return validateSingleType(types);
}
}  // namespace mongo::c_node_validation
