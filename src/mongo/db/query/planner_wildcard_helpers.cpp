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


#include <absl/container/node_hash_set.h>
#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/optional.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/index_path_projection.h"
#include "mongo/db/exec/projection_executor_utils.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/interval/interval.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/index_multikey_helpers.h"
#include "mongo/db/query/planner_wildcard_helpers.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
namespace wildcard_planning {
namespace {
/**
 * Returns a new key pattern object with '$_path' and finds the wildcard field name.
 */
BSONObj makeNewKeyPattern(const IndexEntry* index, StringData* wildcardFieldName) {
    BSONObjBuilder newPattern;
    size_t idx = 0;
    for (auto elem : index->keyPattern) {
        if (idx == index->wildcardFieldPos) {
            newPattern.append(BSON("$_path" << 1).firstElement());
            *wildcardFieldName = elem.fieldNameStringData();
        }
        newPattern.append(elem);
        idx++;
    }
    return newPattern.obj();
}

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
                                      const MultikeyComponents& multikeyPathComponents) {
    // Can't be equal if 'staticComparisonPath' has more parts than 'fieldNameOrArrayIndexPath'.
    if (staticComparisonPath.numParts() > fieldNameOrArrayIndexPath.numParts()) {
        return false;
    }
    auto comparisonPathIndex = 0;         // Index into staticComparisonPath.
    auto parentComponentIsArray = false;  // Whether the parent path component is an array.
    for (auto pathIndex = 0;              // Index into fieldNameOrArrayIndexPath.
         pathIndex < fieldNameOrArrayIndexPath.numParts();
         ++pathIndex) {
        auto incrementComparisonPathIndexBy = 1;
        if (comparisonPathIndex >= staticComparisonPath.numParts()) {
            // If we exceed the bounds of the static comparison path, then clearly the path doesn't
            // match because of the trailing path components.
            return false;
        } else if (fieldNameOrArrayIndexPath.getPart(pathIndex) ==
                   staticComparisonPath.getPart(comparisonPathIndex)) {
            // Path component matches the comparison path verbatim. Check passes, move on to the
            // next component.
        } else if (parentComponentIsArray &&
                   fieldNameOrArrayIndexPath.isNumericPathComponentStrict(pathIndex)) {
            // Check whether the path component could be an array indexing component. Since
            // parentComponentIsArray is never true on the first iteration, this will not underflow.
            // We then decrease the comparison path index to account to re-compare against the field
            // in the array field path.
            incrementComparisonPathIndexBy = 0;
        } else {
            // Because the path is neither a verbatim match nor could it be an array index
            // reference, the path does not match.
            return false;
        }
        // Cache whether this path component, soon to be the parent path component, is array-ish.
        parentComponentIsArray = multikeyPathComponents.contains(pathIndex);
        comparisonPathIndex += incrementComparisonPathIndexBy;
    }

    // Ensure that we matched the entire 'staticComparisonPath' dotted string.
    return staticComparisonPath.numParts() == comparisonPathIndex;
}

/**
 * Returns true if 'multikeyPathSet' contains a FieldRef that matches 'pathToLookup' exactly, or
 * matches 'pathToLookup' when the latter's array indices are ignored.
 */
bool fieldNameOrArrayIndexPathSetContains(const std::set<FieldRef>& multikeyPathSet,
                                          const MultikeyComponents& multikeyPathComponents,
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
 * Returns a FieldRef of the full dotted field, minus the parts at indices listed in
 * 'skipComponents'.
 */
FieldRef pathWithoutSpecifiedComponents(const FieldRef& path,
                                        const std::set<size_t>& skipComponents) {
    // If 'skipComponents' is empty, we return 'path' immediately.
    if (skipComponents.empty()) {
        return path;
    }
    FieldRef result;
    for (size_t index = 0; index < path.numParts(); ++index) {
        if (!skipComponents.count(index)) {
            result.appendPart(path.getPart(index));
        }
    }
    return result;
}

/**
 * Returns a MultikeyPaths which indicates which components of 'indexedPath' are multikey, by
 * looking up multikeyness in 'multikeyPathSet'.
 */
MultikeyPaths buildMultiKeyPathsForExpandedWildcardIndexEntry(
    const BSONObj& keyPattern,
    const FieldRef& indexedPath,
    const std::set<FieldRef>& multikeyPathSet) {
    MultikeyPaths multikeyPaths{};
    for (const auto& field : keyPattern) {
        if (WildcardNames::isWildcardFieldName(field.fieldNameStringData())) {
            FieldRef pathToLookup;
            MultikeyComponents mkComponents;
            for (size_t i = 0; i < indexedPath.numParts(); ++i) {
                pathToLookup.appendPart(indexedPath.getPart(i));
                if (fieldNameOrArrayIndexPathSetContains(
                        multikeyPathSet, mkComponents, pathToLookup)) {
                    mkComponents.insert(i);
                }
            }
            multikeyPaths.emplace_back(mkComponents);
        } else {
            multikeyPaths.emplace_back();
        }
    }
    return multikeyPaths;
}

std::set<FieldRef> generateFieldNameOrArrayIndexPathSet(const MultikeyComponents& multikeyPaths,
                                                        const FieldRef& queryPath,
                                                        bool requiresSubpathBounds) {
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

        // Add the path to the FieldRef set, and obtain an iterator pointing to the new entry.
        const auto result =
            paths.emplace(pathWithoutSpecifiedComponents(queryPath, arrayIndicesToSkip));

        // If any path in the set prefixes another, then the bounds generated will overlap (and
        // thus, be invalid). So, we must make sure that the new path does not prefix and is not
        // prefixed by any existing entries in the set. If any such prefixes do exist, we must
        // remove the subpath(s) and retain only the shortest prefix path, since the bounds it
        // generates will be a superset of all the paths generated by the removed entries.
        if (requiresSubpathBounds && result.second) {
            const auto currentPathItr = result.first;
            // If the new path is a subpath of an existing entry, remove the new path.
            if (currentPathItr != paths.begin() &&
                std::prev(currentPathItr)->isPrefixOf(*currentPathItr)) {
                paths.erase(currentPathItr);
                continue;
            }
            // If existing paths are subpaths of the new entry, remove the old paths.
            while (std::next(currentPathItr) != paths.end() &&
                   currentPathItr->isPrefixOf(*std::next(currentPathItr))) {
                paths.erase(std::next(currentPathItr));
            }
        }
    }
    return paths;
}

/**
 * Returns false if 'queryPath' includes any numerical path components which render it unanswerable
 * by the $** index, true otherwise. Specifically, the $** index cannot answer the query if any
 * of the following scenarios occur:
 *
 * - The query path traverses through more than 'kWildcardMaxArrayIndexTraversalDepth' nested arrays
 * via explicit array indices.
 * - The query path has multiple successive positional components that come immediately after a
 *   multikey path component.
 * - The query path lies along a $** projection through an array index.
 *
 * For an example of the latter case, say that our query path is 'a.0.b', our projection includes
 * {'a.0': 1}, and 'a' is multikey. The query semantics will match 'a.0.b' by either field name or
 * array index against the documents, but because the $** index always projects numeric path
 * components strictly as field names, the projection {'a.0': 1} cannot correctly support this
 * query.
 *
 * To see why, consider the document {a: [1, 2, 3]}. Query {'a.0': 1} will match this document, but
 * the projection {'a.0': 1} will produce output document {a: []}, and so we will not index any of
 * the values [1, 2, 3] for 'a'.
 */
bool validateNumericPathComponents(const MultikeyPaths& multikeyPaths,
                                   const std::set<FieldRef>& includedPaths,
                                   const FieldRef& queryPath) {
    // Find the position of the Wildcard's MultikeyComponents in the paths, we assume that the
    // wildcard field is the only one that can be multikey.
    auto wildcardComponent = std::find_if(multikeyPaths.begin(),
                                          multikeyPaths.end(),
                                          [](const MultikeyComponents& c) { return !c.empty(); });
    if (wildcardComponent == multikeyPaths.end()) {
        // If no MultikeyComponents just return.
        return true;
    }

    // Find the positions of all multikey path components in 'queryPath' that have a numerical path
    // component immediately after. For a queryPath of 'a.2.b' this will return position 0; that is,
    // 'a'. If no such multikey path was found, we are clear to proceed with planning.
    const auto arrayIndices = findArrayIndexPathComponents(*wildcardComponent, queryPath);
    if (arrayIndices.empty()) {
        return true;
    }
    // To support $** fieldname-or-array-index semantics, the planner must generate the power set of
    // all paths with and without array indices. Because this is O(2^n), we decline to answer
    // queries that traverse more than 8 levels of array indices.
    if (arrayIndices.size() > kWildcardMaxArrayIndexTraversalDepth) {
        LOGV2_DEBUG(20955,
                    2,
                    "Declining to answer query on a field with $** index, as it traverses through "
                    "more than the maximum permitted depth of nested array indices",
                    "field"_attr = queryPath.dottedField(),
                    "maxNestedArrayIndices"_attr = kWildcardMaxArrayIndexTraversalDepth);
        return false;
    }

    // Prevent the query from attempting to use a wildcard index if there are multiple successive
    // positional path components that follow a multikey path component. For example, the path
    // "a.0.1" cannot use a wildcard index if "a" is multikey.
    //
    // This restriction stems from the fact that wildcard indices do not recursively index nested
    // arrays. The document {a: [[3, 4]]}, for instance, will have a single index key containing
    // the array [3, 4] rather than individual index keys for 3 and 4. If "a" is known to be
    // multikey, then a user could issue a query like {"a.0.1": {$eq: 4}} to attempt to match by
    // position within a nested array. The access planner is not able to generate useful bounds for
    // such positional queries over nested arrays.
    //
    // We have already found all positional path components that are immediately preceded by a
    // multikey path component. All that remains is to bail out if any of these positional path
    // components are followed by another positional component.
    for (auto&& positionalComponentIndex : arrayIndices) {
        auto adjacentIndex = positionalComponentIndex + 1;
        if (adjacentIndex < queryPath.numParts() &&
            queryPath.isNumericPathComponentStrict(adjacentIndex)) {
            // There are two adjacent positional components, so this query might need to match by
            // position in a nested array. The query cannot be answered using a wildcard index.
            return false;
        }
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

/**
 * Returns expanded wildcard key pattern with a wildcard field replaced by the given expandField and
 * the position of the replaced wildcard field.
 */
std::pair<BSONObj, size_t> expandWildcardIndexKeyPattern(const BSONObj& wildcardKeyPattern,
                                                         StringData expandFieldName) {
    int wildcardFieldPos = -1;
    int fieldPos = 0;
    BSONObjBuilder builder{};
    for (const auto& field : wildcardKeyPattern) {
        const auto& fieldName = field.fieldNameStringData();
        if (WildcardNames::isWildcardFieldName(fieldName)) {
            tassert(7246500,
                    str::stream()
                        << "Wildcard Index's key pattern must contain exactly one wildcard field: '"
                        << wildcardKeyPattern << "'.",
                    wildcardFieldPos < 0);
            builder.appendAs(field, expandFieldName);
            wildcardFieldPos = fieldPos;
        } else {
            builder.append(field);
        }
        ++fieldPos;
    }

    tassert(7246501,
            str::stream() << "Wildcard Index's key pattern must contain one wildcard field: '"
                          << wildcardKeyPattern << "'.",
            wildcardFieldPos >= 0);

    return std::make_pair(builder.obj(), static_cast<size_t>(wildcardFieldPos));
}

boost::optional<IndexEntry> createExpandedIndexEntry(const IndexEntry& wildcardIndex,
                                                     const std::string& fieldName,
                                                     const std::set<FieldRef>& includedPaths) {
    // Convert string 'fieldName' into a FieldRef, to better facilitate the subsequent checks.
    auto queryPath = FieldRef{fieldName};
    // $** indices hold multikey metadata directly in the index keys, rather than in the index
    // catalog. In turn, the index key data is used to produce a set of multikey paths
    // in-memory. Here we convert this set of all multikey paths into a MultikeyPaths vector
    // which will indicate to the downstream planning code which components of 'fieldName' are
    // multikey.
    auto multikeyPaths = buildMultiKeyPathsForExpandedWildcardIndexEntry(
        wildcardIndex.keyPattern, queryPath, wildcardIndex.multikeyPathSet);

    // Check whether a query on the current fieldpath is answerable by the $** index, given any
    // numerical path components that may be present in the path string.
    if (!validateNumericPathComponents(multikeyPaths, includedPaths, queryPath)) {
        return boost::none;
    }

    // The expanded IndexEntry is only considered multikey if the particular path represented by
    // this IndexEntry has a multikey path component. For instance, suppose we have index {$**:
    // 1} with "a" as the only multikey path. If we have a query on paths "a.b" and "c.d", then
    // we will generate two expanded index entries: one for "a.b" and "c.d". The "a.b" entry
    // will be marked as multikey because "a" is multikey, whereas the "c.d" entry will not be
    // marked as multikey.
    tassert(7246506,
            "multikeyPaths size must be equal to the number of the key pattern fields.",
            multikeyPaths.size() == static_cast<size_t>(wildcardIndex.keyPattern.nFields()));

    auto [expandedKeyPattern, wildcardFieldPos] =
        expandWildcardIndexKeyPattern(wildcardIndex.keyPattern, fieldName);
    const bool isMultikey = !multikeyPaths[wildcardFieldPos].empty();
    IndexEntry entry(std::move(expandedKeyPattern),
                     IndexType::INDEX_WILDCARD,
                     IndexConfig::kLatestIndexVersion,
                     isMultikey,
                     std::move(multikeyPaths),
                     // Expanded index entries always use the fixed-size multikey paths
                     // representation, so we purposefully discard 'multikeyPathSet'.
                     {},
                     true,   // sparse
                     false,  // unique
                     {wildcardIndex.identifier.catalogName, fieldName},
                     wildcardIndex.filterExpr,
                     wildcardIndex.infoObj,
                     wildcardIndex.collator,
                     wildcardIndex.indexPathProjection,
                     wildcardIndex.indexCatalogEntryStorage,
                     wildcardFieldPos);
    return entry;
}

/**
 * Determines if an expanded index entry can satisfy a query on a wildcard field with a FETCH
 * (for e.g., it may only be able to answer a query on the prefix if the wildcard field is being
 * queried with an incompatible $not predicate).
 *
 * Note: we could just use 'index.keyPattern' here for this check, but then we would have to iterate
 * through the entire pattern to get to the field at 'wildcardPos'.
 */
bool canOnlyAnswerWildcardPrefixQuery(const IndexEntry& index, const IndexBounds& bounds) {
    tassert(7444000, "Expected a wildcard index.", index.type == INDEX_WILDCARD);
    tassert(7444001,
            "A wildcard index should always have a virtual $_path field at wildcardFieldPos - 1.",
            bounds.fields[index.wildcardFieldPos - 1].name == "$_path"_sd);

    if (index.wildcardFieldPos == 1) {
        // This is either a single-field wildcard index, or a compound wildcard index without a
        // prefix.
        return false;
    }

    // If the index entry was not expanded to include a second $_path field, we cannot answer a
    // query on a wildcard field with an IXSCAN + FETCH if the predicate itself is, for e.g. an
    // ineligible $not query, because we won't retrieve documents where the wildcard field is
    // missing from the IXSCAN.
    return bounds.fields[index.wildcardFieldPos].name != "$_path"_sd;
}
}  // namespace

void expandWildcardIndexEntry(const IndexEntry& wildcardIndex,
                              const std::set<std::string>& fields,
                              std::vector<IndexEntry>* out) {
    tassert(7246502, "out parameter cannot be null", out);
    tassert(7246503,
            "expandWildcardIndexEntry expected only WildcardIndexes",
            wildcardIndex.type == INDEX_WILDCARD);

    // $** indexes do not keep the multikey metadata inside the index catalog entry, as the amount
    // of metadata is not bounded. We do not expect IndexEntry objects for $** indexes to have a
    // fixed-size vector of multikey metadata until after they are expanded.
    tassert(7246504,
            "multikeyPaths must be empty for Wildcard Indexes",
            wildcardIndex.multikeyPaths.empty());

    // Obtain the projection executor from the parent wildcard IndexEntry.
    auto* wildcardProjection = wildcardIndex.indexPathProjection;
    tassert(
        7246505, "wildcardProjection must be non-null for Wildcard Indexes", wildcardProjection);

    const auto projectedFields =
        projection_executor_utils::applyProjectionToFields(wildcardProjection->exec(), fields);

    const static auto kEmptySet = std::set<FieldRef>{};
    const auto& includedPaths =
        wildcardProjection->exhaustivePaths() ? *wildcardProjection->exhaustivePaths() : kEmptySet;

    for (auto&& fieldName : projectedFields) {
        auto entry = createExpandedIndexEntry(wildcardIndex, fieldName, includedPaths);

        if (entry == boost::none) {
            continue;
        }
        tassert(7246507,
                "'$_path' is reserved fieldname for Wildcard Indexes",
                "$_path"_sd != fieldName);
        out->push_back(*entry);
    }

    // If this wildcard index cannot be expanded because the wildcard field is not relevant. We
    // should also check whether the regular fields is able to answer the query or not. That is - if
    // any field of the regular fields in a compound wildcard index is in 'fields', then we should
    // also generate an expanded wildcard 'IndexEntry' for later index analysis.
    bool shouldExpand = false;
    for (auto elem : wildcardIndex.keyPattern) {
        auto fieldName = elem.fieldNameStringData();
        if (WildcardNames::isWildcardFieldName(fieldName)) {
            break;
        }
        if (fields.count(std::string{fieldName})) {
            shouldExpand = true;
            break;
        }
    }

    // This expanded IndexEntry is for queries on the non-wildcard prefix of a compound wildcard
    // index, the wildcard component is not required. We use the reserved path, "$_path", to
    // instruct the query planner to generate "all values" index bounds and not consider this
    // field in supporting any sort operation.
    if (shouldExpand) {
        auto entry = createExpandedIndexEntry(wildcardIndex, "$_path", {} /* paths included */);
        out->push_back(*entry);
    }
}

bool canOnlyAnswerWildcardPrefixQuery(
    const std::vector<std::unique_ptr<QuerySolutionNode>>& ixscanNodes) {
    return std::any_of(ixscanNodes.begin(), ixscanNodes.end(), [](const auto& node) {
        if (node->getType() == StageType::STAGE_IXSCAN) {
            const auto* ixScanNode = static_cast<IndexScanNode*>(node.get());
            const auto& index = ixScanNode->index;
            if (index.type == INDEX_WILDCARD &&
                canOnlyAnswerWildcardPrefixQuery(index, ixScanNode->bounds)) {
                return true;
            }
        }
        return false;
    });
}

void finalizeWildcardIndexScanConfiguration(
    IndexScanNode* scan, std::vector<interval_evaluation_tree::Builder>* ietBuilders) {
    IndexEntry* index = &scan->index;
    IndexBounds* bounds = &scan->bounds;

    // We should only ever reach this point when processing a $** index. Sanity check the arguments.
    invariant(index && index->type == IndexType::INDEX_WILDCARD);

    // For $** indexes, the IndexEntry key pattern is {..., 'path.to.field': 1, ...} but the actual
    // keys in the index are of the form {..., '$_path': 1, 'path.to.field': 1, ...}, where the
    // value of the wildcard field in each key is 'path.to.field'. We push a new entry into the
    // bounds vector for the leading '$_path' bound here. We also push corresponding fields into the
    // IndexScanNode's keyPattern and its multikeyPaths vector.
    index->multikeyPaths.insert(index->multikeyPaths.begin() + index->wildcardFieldPos,
                                MultikeyComponents{});
    bounds->fields.insert(bounds->fields.begin() + index->wildcardFieldPos, {"$_path"});

    StringData wildcardFieldName;
    index->keyPattern = makeNewKeyPattern(index, &wildcardFieldName);

    if (!ietBuilders->empty()) {
        auto wildcardIt = ietBuilders->begin();
        std::advance(wildcardIt, index->wildcardFieldPos);
        ietBuilders->emplace(wildcardIt);
    }

    // Update the position as we insert "$_path" prior to the wildcard field.
    index->wildcardFieldPos++;

    // If the wildcard field is "$_path", the index is used to answer query only on the non-wildcard
    // prefix of a compound wildcard index. The bounds for the "$_path" field should scan all
    // string values and 'MinKey'. The bounds for the generic wildcard field should scan all values
    // with bounds, "[MinKey, MaxKey]". Because the wildcard field can generate multiple keys for
    // one single document, we should also instruct the IXSCAN to dedup keys.
    if (wildcardFieldName == "$_path"_sd) {
        bounds->fields[index->wildcardFieldPos - 1].intervals = makeAllValuesForPath();
        bounds->fields[index->wildcardFieldPos].intervals.push_back(
            IndexBoundsBuilder::allValues());
        bounds->fields[index->wildcardFieldPos].name = "$_path";
        scan->shouldDedup = true;
        return;
    }

    // Create a FieldRef to perform any necessary manipulations on the query path string.
    FieldRef queryPath{wildcardFieldName};
    auto& multikeyPaths = index->multikeyPaths[index->wildcardFieldPos];

    // If the bounds overlap the object type bracket or the wildcard field's bounds were not filled,
    // then we must retrieve all documents which include the given path. We must therefore add
    // bounds that encompass all its subpaths, specifically the interval ["path.","path/") on
    // "$_path".
    const bool requiresSubpathBounds = bounds->fields[index->wildcardFieldPos].name.empty() ||
        bounds->fields[index->wildcardFieldPos].boundsOverlapObjectTypeBracket();

    // Account for fieldname-or-array-index semantics. $** indexes do not explicitly encode array
    // indices in their keys, so if this query traverses one or more multikey fields via an array
    // index (e.g. query 'a.0.b' where 'a' is an array), then we must generate bounds on all array-
    // and non-array permutations of the path in order to produce INEXACT_FETCH bounds.
    auto paths =
        generateFieldNameOrArrayIndexPathSet(multikeyPaths, queryPath, requiresSubpathBounds);

    // Add a $_path point-interval for each path that needs to be traversed in the index. If subpath
    // bounds are required, then we must add a further range interval on ["path.","path/").
    static const char subPathStart = '.', subPathEnd = static_cast<char>('.' + 1);
    auto& pathIntervals = bounds->fields[index->wildcardFieldPos - 1].intervals;
    for (const auto& fieldPath : paths) {
        auto path = std::string{fieldPath.dottedField()};
        pathIntervals.push_back(IndexBoundsBuilder::makePointInterval(path));
        if (requiresSubpathBounds) {
            pathIntervals.push_back(IndexBoundsBuilder::makeRangeInterval(
                path + subPathStart, path + subPathEnd, BoundInclusion::kIncludeStartKeyOnly));

            // Queries which scan subpaths for a single wildcard index should be deduped. The index
            // bounds may include multiple keys associated with the same document. Therefore, we
            // instruct the IXSCAN to dedup keys which point to the same object.
            scan->shouldDedup = true;
        }
    }
}

std::vector<Interval> makeAllValuesForPath() {
    std::vector<Interval> intervals;

    // Generating a [MinKey, MinKey] point interval. We use 'MinKey' as the "$_path" key value for
    // documents that don't have any wildcard field.
    BSONObjBuilder minKeyBob;
    minKeyBob.appendMinKey("");
    intervals.push_back(IndexBoundsBuilder::makePointInterval(minKeyBob.obj()));

    // Generating a all-value index bounds for only string type, because "$_path" with a string
    // value tracks the wildcard path.
    BSONObjBuilder allStringBob;
    allStringBob.appendMinForType("", stdx::to_underlying(BSONType::string));
    allStringBob.appendMaxForType("", stdx::to_underlying(BSONType::string));
    intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        allStringBob.obj(), BoundInclusion::kIncludeStartKeyOnly));

    return intervals;
}

}  // namespace wildcard_planning
}  // namespace mongo
