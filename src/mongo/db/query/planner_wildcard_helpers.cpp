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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/planner_wildcard_helpers.h"

#include <vector>

#include "mongo/bson/util/builder.h"
#include "mongo/util/log.h"

namespace mongo {
namespace wildcard_planning {
namespace {
/**
 * Compares the path 'fieldNameOrArrayIndexPath' to 'staticComparisonPath', ignoring any array
 * indices present in the former if they are not present in the latter. The 'multikeyPathComponents'
 * set contains the path positions that are known to be arrays; only numerical path components that
 * immediately follow an array field are considered array indices. If 'fieldNameOrArrayIndexPath' is
 * 'a.0.b', it will match 'staticComparisonPath' 'a.0.b', and it will also match 'a.b' but only if
 * 'multikeyPathComponents' indicates that 'a' is an array.
 */
bool fieldNameOrArrayIndexPathMatches(const FieldRef& fieldNameOrArrayIndexPath,
                                      const FieldRef& staticComparisonPath,
                                      const std::set<size_t>& multikeyPathComponents) {
    // Can't be equal if 'staticComparisonPath' has more parts than 'fieldNameOrArrayIndexPath'.
    if (staticComparisonPath.numParts() > fieldNameOrArrayIndexPath.numParts()) {
        return false;
    }
    size_t offset = 0;
    for (size_t i = 0; i < fieldNameOrArrayIndexPath.numParts(); ++i) {
        if (i - offset >= staticComparisonPath.numParts()) {
            return false;
        }
        if (fieldNameOrArrayIndexPath.getPart(i) == staticComparisonPath.getPart(i - offset)) {
            continue;
        } else if (multikeyPathComponents.count(i - 1) &&
                   fieldNameOrArrayIndexPath.isNumericPathComponent(i)) {
            ++offset;
            continue;
        }
        return false;
    }
    // Ensure that we matched the entire 'staticComparisonPath' dotted string.
    return fieldNameOrArrayIndexPath.numParts() == staticComparisonPath.numParts() + offset;
}

/**
 * Returns true if 'multikeyPathSet' contains a FieldRef that matches 'pathToLookup' exactly, or
 * matches 'pathToLookup' when the latter's array indices are ignored.
 */
bool fieldNameOrArrayIndexPathSetContains(const std::set<FieldRef>& multikeyPathSet,
                                          const std::set<std::size_t>& multikeyPathComponents,
                                          const FieldRef& pathToLookup) {
    // Fast-path check for an exact match. If there is no exact match and 'pathToLookup' has no
    // numeric path components, then 'multikeyPathSet' does not contain the path.
    if (multikeyPathSet.count(pathToLookup)) {
        return true;
    } else if (!pathToLookup.hasNumericPathComponents()) {
        return false;
    }
    // Determine whether any of the 'multikeyPathSet' entries match 'pathToLookup' under relaxed
    // fieldname-or-array-index constraints.
    return std::any_of(
        multikeyPathSet.begin(), multikeyPathSet.end(), [&](const auto& multikeyPath) {
            return fieldNameOrArrayIndexPathMatches(
                pathToLookup, multikeyPath, multikeyPathComponents);
        });
}

/**
 * Returns the positions of all path components in 'queryPath' that may be interpreted as array
 * indices by the query system. We obtain this list by finding all multikey path components that
 * have a numerical path component immediately after.
 */
std::vector<size_t> findArrayIndexPathComponents(const std::set<std::size_t>& multikeyPaths,
                                                 const FieldRef& queryPath) {
    std::vector<size_t> arrayIndices;
    for (auto i : multikeyPaths) {
        if (i < queryPath.numParts() - 1 && queryPath.isNumericPathComponent(i + 1)) {
            arrayIndices.push_back(i + 1);
        }
    }
    return arrayIndices;
}

/**
 * Returns an std::string of the full dotted field, minus the parts listed in 'skipParts'.
 */
FieldRef pathWithoutSpecifiedComponents(const FieldRef& path,
                                        const std::set<size_t>& skipComponents) {
    // If 'skipComponents' is empty, we return 'path' immediately.
    if (skipComponents.empty()) {
        return path;
    }
    StringBuilder ss;
    size_t startPart = 0;
    for (const auto& skipPart : skipComponents) {
        ss << (ss.len() && !ss.stringData().endsWith(".") ? "." : "")
           << path.dottedSubstring(startPart, skipPart);
        startPart = skipPart + 1;
    }
    if (startPart < path.numParts()) {
        ss << (ss.len() && !ss.stringData().endsWith(".") ? "." : "")
           << path.dottedSubstring(startPart, path.numParts());
    }
    return FieldRef{ss.str()};
}
}  // namespace

MultikeyPaths buildMultiKeyPathsForExpandedWildcardIndexEntry(
    const FieldRef& indexedPath, const std::set<FieldRef>& multikeyPathSet) {
    FieldRef pathToLookup;
    std::set<std::size_t> multikeyPaths;
    for (size_t i = 0; i < indexedPath.numParts(); ++i) {
        pathToLookup.appendPart(indexedPath.getPart(i));
        if (fieldNameOrArrayIndexPathSetContains(multikeyPathSet, multikeyPaths, pathToLookup)) {
            multikeyPaths.insert(i);
        }
    }
    return {multikeyPaths};
}

std::set<FieldRef> generateFieldNameOrArrayIndexPathSet(const std::set<std::size_t>& multikeyPaths,
                                                        const FieldRef& queryPath) {
    // We iterate over the power set of array index positions to generate all necessary paths.
    // The algorithm is unavoidably O(n2^n), but we enforce that 'n' is never more than single
    // digits during the planner's index selection phase.
    const auto potentialArrayIndices = findArrayIndexPathComponents(multikeyPaths, queryPath);
    invariant(potentialArrayIndices.size() <= kWildcardMaxArrayIndexTraversalDepth);
    invariant(potentialArrayIndices.size() < sizeof(size_t) * 8u);
    // We iterate over every value [0..2^n), where 'n' is the size of 'potentialArrayIndices',
    // treating each value as a 'bitMask' of 'n' bits. Each bit in 'bitMask' represents the
    // entry at the equivalent position in the 'potentialArrayIndices' vector. When a given bit
    // is set, we treat the corresponding numeric path component as an array index, and generate
    // a path that omits it. When a bit is not set, we treat the numeric path component as a
    // literal fieldname, and we generate a path that includes it. Because we iterate over every
    // value [0..2^n), we ensure that we generate every combination of 'n' bits, and therefore
    // every possible fieldname and array index path.
    std::set<FieldRef> paths;
    for (size_t bitMask = 0; bitMask < (size_t{1} << potentialArrayIndices.size()); ++bitMask) {
        std::set<size_t> arrayIndicesToSkip;
        for (size_t i = 0; i < potentialArrayIndices.size(); ++i) {
            if (bitMask & (size_t{1} << i)) {
                arrayIndicesToSkip.insert(potentialArrayIndices[i]);
            }
        }
        paths.insert(pathWithoutSpecifiedComponents(queryPath, arrayIndicesToSkip));
    }
    return paths;
}

BoundsTightness applyWildcardIndexScanBoundsTightness(const IndexEntry& index,
                                                      BoundsTightness tightnessIn) {
    // This method should only ever be called for a $** IndexEntry. We expect to be called during
    // planning, *before* finishWildcardIndexScanNode has been invoked. The IndexEntry should thus
    // only have a single keyPattern field and multikeyPath entry, but this is sufficient to
    // determine whether it will be necessary to adjust the tightness.
    invariant(index.type == IndexType::INDEX_WILDCARD);
    invariant(index.keyPattern.nFields() == 1);
    invariant(index.multikeyPaths.size() == 1);

    // If the tightness is already INEXACT_FETCH, any further changes are redundant.
    if (tightnessIn == BoundsTightness::INEXACT_FETCH) {
        return tightnessIn;
    }

    // If the query passes through any array indices, we must always fetch and filter the documents.
    const auto arrayIndicesTraversedByQuery = findArrayIndexPathComponents(
        index.multikeyPaths.front(), FieldRef{index.keyPattern.firstElementFieldName()});

    // If the list of array indices we traversed is non-empty, set the tightness to INEXACT_FETCH.
    return (arrayIndicesTraversedByQuery.empty() ? tightnessIn : BoundsTightness::INEXACT_FETCH);
}

bool validateNumericPathComponents(const MultikeyPaths& multikeyPaths,
                                   const std::set<FieldRef>& includedPaths,
                                   const FieldRef& queryPath) {
    // $** multikeyPaths always have a singleton set, since they are single-element indexes.
    invariant(multikeyPaths.size() == 1);

    // Find the positions of all multikey path components in 'queryPath' that have a numerical path
    // component immediately after. For a queryPath of 'a.2.b' this will return position 0; that is,
    // 'a'. If no such multikey path was found, we are clear to proceed with planning.
    const auto arrayIndices = findArrayIndexPathComponents(multikeyPaths.front(), queryPath);
    if (arrayIndices.empty()) {
        return true;
    }
    // To support $** fieldname-or-array-index semantics, the planner must generate the power set of
    // all paths with and without array indices. Because this is O(2^n), we decline to answer
    // queries that traverse more than 8 levels of array indices.
    if (arrayIndices.size() > kWildcardMaxArrayIndexTraversalDepth) {
        LOG(2) << "Declining to answer query on field '" << queryPath.dottedField()
               << "' with $** index, as it traverses through more than "
               << kWildcardMaxArrayIndexTraversalDepth << " nested array indices.";
        return false;
    }
    // If 'includedPaths' is empty, then either the $** projection is an exclusion, or no explicit
    // projection was provided. In either case, it is not possible for the query path to lie along
    // an array index projection, and so we are safe to proceed with planning.
    if (includedPaths.empty()) {
        return true;
    }
    // Find the $** projected field which prefixes or is equal to the query path. If 'includedPaths'
    // is non-empty then we are guaranteed that exactly one entry will prefix the query path, since
    // (a) if no such inclusion exists, an IndexEntry would not have been created for this path, and
    // (b) conflicting paths, such as 'a.b' and 'a.b.c', are not permitted in projections.
    auto includePath = std::find_if(
        includedPaths.begin(), includedPaths.end(), [&queryPath](const auto& includedPath) {
            return includedPath.isPrefixOfOrEqualTo(queryPath);
        });
    invariant(std::next(includePath) == includedPaths.end() || *std::next(includePath) > queryPath);

    // If the projectedPath responsible for including this queryPath prefixes it up to and including
    // the numerical array index field, then the queryPath lies along a projection through the array
    // index, and we cannot support the query for the reasons outlined above.
    return arrayIndices[0] >= includePath->numParts();
}

}  // namespace wildcard_planning
}  // namespace mongo
