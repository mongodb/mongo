/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/semantic_analysis.h"

namespace mongo::semantic_analysis {

namespace {

/**
 * If 'pathOfInterest' or some path prefix of 'pathOfInterest' is renamed, returns the rename for
 * 'pathOfInterest', otherwise returns boost::none.  For example, if 'renamedPaths' is {"a", "b"},
 * and 'pathOfInterest' is "a.c", returns "b.c". Note that 'renamedPaths' must map names as they
 * exist at one fixed point in an aggregation pipeline to names as they exist at another fixed point
 * in the same pipeline (i.e. from path names as they exist before some particular aggregation
 * stage, to names as they appear after that stage).
 */
boost::optional<std::string> findRename(const StringMap<std::string>& renamedPaths,
                                        std::string pathOfInterest) {
    FieldPath fullPathOfInterest(pathOfInterest);
    boost::optional<StringBuilder> toLookup;
    boost::optional<StringBuilder> renamedPath;
    for (std::size_t pathIndex = 0; pathIndex < fullPathOfInterest.getPathLength(); ++pathIndex) {
        if (!renamedPath) {
            if (!toLookup) {
                toLookup.emplace();
            } else {
                (*toLookup) << ".";
            }
            (*toLookup) << fullPathOfInterest.getFieldName(pathIndex);
            if (auto it = renamedPaths.find(toLookup->stringData()); it != renamedPaths.end()) {
                renamedPath.emplace();
                (*renamedPath) << it->second;
            }
            // Found a rename - but it might be a rename of the prefix of the path, so we have to
            // add back on the suffix that was unchanged.
        } else {
            (*renamedPath) << "." << fullPathOfInterest.getFieldName(pathIndex);
        }
    }
    return renamedPath.map([](auto&& path) { return path.str(); });
}

/**
 * Computes and returns a map that contains a mapping from each path in 'pathsOfInterest' to its
 * rename, as determined by 'renamedPaths.' If a path was not renamed, assumes it is unmodified and
 * maps the path to itself.
 */
StringMap<std::string> computeNamesAssumingAnyPathsNotRenamedAreUnmodified(
    const StringMap<std::string>& renamedPaths, const OrderedPathSet& pathsOfInterest) {
    StringMap<std::string> renameOut;
    for (auto&& ofInterest : pathsOfInterest) {
        if (auto name = findRename(renamedPaths, ofInterest)) {
            renameOut[ofInterest] = *name;
        } else {
            // This path was not renamed, assume it was unchanged and map it to itself.
            renameOut[ofInterest] = ofInterest;
        }
    }
    return renameOut;
}

StringMap<std::string> invertRenameMap(const StringMap<std::string>& originalMap) {
    StringMap<std::string> reversedMap;
    for (auto&& [newName, oldName] : originalMap) {
        reversedMap[oldName] = newName;
    }
    return reversedMap;
}

const ReplaceRootTransformation* isReplaceRoot(const DocumentSource* source) {
    // We have to use getSourceName() since DocumentSourceReplaceRoot is never materialized - it
    // uses DocumentSourceSingleDocumentTransformation.
    auto singleDocTransform =
        dynamic_cast<const DocumentSourceSingleDocumentTransformation*>(source);
    if (!singleDocTransform) {
        return nullptr;
    }
    return dynamic_cast<const ReplaceRootTransformation*>(&singleDocTransform->getTransformer());
}

/**
 * Detects if 'replaceRootTransform' represents the nesting of a field path. If it does, returns
 * the name of that field path. For example, if 'replaceRootTransform' represents the transformation
 * associated with {$replaceWith: {nested: "$$ROOT"}} or {$replaceRoot: {newRoot: {nested:
 * "$$ROOT"}}}, returns "nested".
 */
boost::optional<std::string> replaceRootNestsRoot(
    const ReplaceRootTransformation* replaceRootTransform) {
    auto expressionObject =
        dynamic_cast<ExpressionObject*>(replaceRootTransform->getExpression().get());
    if (!expressionObject) {
        return boost::none;
    }
    auto children = expressionObject->getChildExpressions();
    if (children.size() != 1u) {
        return boost::none;
    }
    auto&& [nestedName, expression] = children[0];
    if (!dynamic_cast<ExpressionFieldPath*>(expression.get()) ||
        !dynamic_cast<ExpressionFieldPath*>(expression.get())->isROOT()) {
        return boost::none;
    }
    return nestedName;
}

/**
 * Detects if 'replaceRootTransform' represents the unnesting of a field path. If it does, returns
 * the name of that field path. For example, if 'replaceRootTransform' represents the transformation
 * associated with {$replaceWith: "$x"} or {$replaceRoot: {newRoot: "$x"}}, returns "x".
 */
boost::optional<std::string> replaceRootUnnestsPath(
    const ReplaceRootTransformation* replaceRootTransform) {
    auto expressionFieldPath =
        dynamic_cast<ExpressionFieldPath*>(replaceRootTransform->getExpression().get());
    if (!expressionFieldPath) {
        return boost::none;
    }
    return expressionFieldPath->getFieldPathWithoutCurrentPrefix().fullPath();
}

/**
 * Looks for a pattern where the user temporarily nests the whole object, does some computation,
 * then unnests the object. Like so:
 * [{$replaceWith: {nested: "$$ROOT"}}, ..., {$replaceWith: "$nested"}].
 *
 * If this pattern is detected, returns an iterator to the 'second' replace root, whichever is later
 * according to the traversal order.
 */
template <class Iterator>
boost::optional<Iterator> lookForNestUnnestPattern(
    Iterator start,
    Iterator end,
    OrderedPathSet pathsOfInterest,
    const Direction& traversalDir,
    boost::optional<std::function<bool(DocumentSource*)>> additionalStageValidatorCallback) {
    auto replaceRootTransform = isReplaceRoot((*start).get());
    if (!replaceRootTransform) {
        return boost::none;
    }

    auto targetName = traversalDir == Direction::kForward
        ? replaceRootNestsRoot(replaceRootTransform)
        : replaceRootUnnestsPath(replaceRootTransform);
    if (!targetName || targetName->find(".") != std::string::npos) {
        // Bail out early on dotted paths - we don't intend to deal with that complexity here,
        // though we could in the future.
        return boost::none;
    }
    auto nameTestCallback =
        traversalDir == Direction::kForward ? replaceRootUnnestsPath : replaceRootNestsRoot;

    ++start;  // Advance one to go past the first $replaceRoot we just looked at.
    for (; start != end; ++start) {
        replaceRootTransform = isReplaceRoot((*start).get());
        if (!replaceRootTransform) {
            if (additionalStageValidatorCallback &&
                !((*additionalStageValidatorCallback)((*start).get()))) {
                // There was an additional condition which failed - bail out.
                return boost::none;
            }

            auto renames = renamedPaths({*targetName}, **start, traversalDir);
            if (!renames ||
                (renames->find(*targetName) != renames->end() &&
                 (*renames)[*targetName] != *targetName)) {
                // This stage is not a $replaceRoot - and it modifies our nested path
                // ('targetName') somehow.
                return boost::none;
            }
            // This is not a $replaceRoot - but it doesn't impact the nested path, so we continue
            // searching for the unnester.
            continue;
        }
        if (auto nestName = nameTestCallback(replaceRootTransform);
            nestName && *nestName == *targetName) {
            if (additionalStageValidatorCallback &&
                !((*additionalStageValidatorCallback)((*start).get()))) {
                // There was an additional condition which failed - bail out.
                return boost::none;
            }
            return start;
        } else {
            // If we have a replaceRoot which is not the one we're looking for - then it modifies
            // the path we're trying to preserve. As a future enhancement, we maybe could recurse
            // here.
            return boost::none;
        }
    }
    return boost::none;
}

/**
 * Computes and returns a rename mapping for 'pathsOfInterest' over multiple aggregation
 * pipeline stages. The range of pipeline stages we consider renames over is represented by the
 * iterators 'start' and 'end'.
 *
 * If both 'start' and 'end' are reverse iterators, then 'start' should come after 'end' in the
 * pipeline, and 'traversalDir' should be "kBackward," 'pathsOfInterest' should be valid path names
 * after stage 'start.'
 *
 * If both 'start' and 'end' are forwards iterators, then 'start' should come before 'end' in the
 * pipeline, 'traversalDir' should be "kForward," and 'pathsOfInterest' should be valid path names
 * before stage 'start.'
 *
 * This function will compute an iterator pointing to the "last" stage (farthest in the given
 * direction, not included) which preserves 'pathsOfInterest' allowing renames, and returns that
 * iterator and a mapping from the given names of 'pathsOfInterest' to their names as they were
 * directly "before" (just previous to, according to the direction) the result iterator. If all
 * stages preserve the paths of interest, returns 'end.'
 *
 * An optional 'additionalStageValidatorCallback' function can be provided to short-circuit this
 * process and return an iterator to the first stage which either (a) does not preserve
 * 'pathsOfInterest,' as before, or (b) does not meet this callback function's criteria.
 *
 * This should only be used internally; callers who need to track path renames through an
 * aggregation pipeline should use one of the publically exposed options available in the header.
 */
template <class Iterator>
std::pair<Iterator, StringMap<std::string>> multiStageRenamedPaths(
    Iterator start,
    Iterator end,
    OrderedPathSet pathsOfInterest,
    const Direction& traversalDir,
    boost::optional<std::function<bool(DocumentSource*)>> additionalStageValidatorCallback =
        boost::none) {
    // The keys to this map will always be the original names of 'pathsOfInterest'. The values
    // will be updated as we loop through the pipeline's stages to always be the most up-to-date
    // name we know of for that path.
    StringMap<std::string> renameMap;
    for (auto&& path : pathsOfInterest) {
        renameMap[path] = path;
    }
    for (; start != end; ++start) {
        if (additionalStageValidatorCallback &&
            !((*additionalStageValidatorCallback)((*start).get()))) {
            // There was an additional condition which failed - bail out.
            return {start, renameMap};
        }

        auto renamed = renamedPaths(pathsOfInterest, **start, traversalDir);
        if (!renamed) {
            if (auto finalReplaceRoot = lookForNestUnnestPattern<Iterator>(
                    start, end, pathsOfInterest, traversalDir, additionalStageValidatorCallback)) {
                // We've just detected a pattern where the user temporarily nests the whole
                // object, does some computation, then unnests the object. Like so:
                // [{$replaceWith: {nested: "$$ROOT"}}, ..., {$replaceWith: "$nested"}].
                // This analysis makes sure that the middle stages don't modify 'nested' or
                // whatever the nesting field path is and the additional callback function's
                // criteria is met. In this case, we can safely skip over all intervening stages and
                // continue on our way.
                start = *finalReplaceRoot;
                continue;
            }
            return {start, renameMap};
        }
        //'pathsOfInterest' always holds the current names of the paths we're interested in, so
        // it needs to be updated after each stage.
        pathsOfInterest.clear();
        for (auto it = renameMap.cbegin(); it != renameMap.cend(); ++it) {
            renameMap[it->first] = (*renamed)[it->second];
            pathsOfInterest.emplace(it->second);
        }
    }
    return {end, renameMap};
}
template <class Iterator>
boost::optional<StringMap<std::string>> renamedPathsFullPipeline(
    Iterator start,
    Iterator end,
    OrderedPathSet pathsOfInterest,
    const Direction& traversalDir,
    boost::optional<std::function<bool(DocumentSource*)>> additionalStageValidatorCallback) {
    auto [itr, renameMap] = multiStageRenamedPaths(
        start, end, pathsOfInterest, traversalDir, additionalStageValidatorCallback);
    if (itr != end) {
        return boost::none;  // The paths were not preserved to the very end.
    }
    return renameMap;
}

}  // namespace

OrderedPathSet extractModifiedDependencies(const OrderedPathSet& dependencies,
                                           const OrderedPathSet& preservedPaths) {
    OrderedPathSet modifiedDependencies;

    // The modified dependencies is *almost* the set difference 'dependencies' - 'preservedPaths',
    // except that if p in 'preservedPaths' is a "path prefix" of d in 'dependencies', then 'd'
    // should not be included in the modified dependencies.
    for (auto&& dependency : dependencies) {
        bool preserved = false;
        auto firstField = FieldPath::extractFirstFieldFromDottedPath(dependency).toString();
        // Because a path is preserved if the object it points to is preserved, if a prefix to a
        // path is preserved, then the path itself must be preserved. So we search for any prefixes
        // of 'dependency' as well. 'preservedPaths' is an *ordered* set, so we only have to search
        // the range ['firstField', 'dependency'] to find any prefixes of 'dependency'.
        for (auto it = preservedPaths.lower_bound(firstField);
             it != preservedPaths.upper_bound(dependency);
             ++it) {
            if (*it == dependency || expression::isPathPrefixOf(*it, dependency)) {
                preserved = true;
                break;
            }
        }
        if (!preserved) {
            modifiedDependencies.insert(dependency);
        }
    }
    return modifiedDependencies;
}

boost::optional<StringMap<std::string>> renamedPaths(const OrderedPathSet& pathsOfInterest,
                                                     const DocumentSource& stage,
                                                     const Direction& traversalDir) {
    auto modifiedPathsRet = stage.getModifiedPaths();
    switch (modifiedPathsRet.type) {
        case DocumentSource::GetModPathsReturn::Type::kNotSupported:
        case DocumentSource::GetModPathsReturn::Type::kAllPaths:
            return boost::none;
        case DocumentSource::GetModPathsReturn::Type::kFiniteSet: {
            // Any overlap of the path means the path of interest is not preserved. For
            // example, if the path of interest is "a.b", then a modified path of "a",
            // "a.b", or "a.b.c" would all signal that "a.b" is not preserved.
            if (!expression::areIndependent(modifiedPathsRet.paths, pathsOfInterest)) {
                return boost::none;
            }

            // None of the paths of interest were modified, construct the result map, mapping
            // the names after this stage to the names before this stage.
            auto renameMap = (traversalDir == Direction::kForward)
                ? invertRenameMap(modifiedPathsRet.renames)
                : modifiedPathsRet.renames;
            return computeNamesAssumingAnyPathsNotRenamedAreUnmodified(renameMap, pathsOfInterest);
        }
        case DocumentSource::GetModPathsReturn::Type::kAllExcept: {
            auto preservedPaths = modifiedPathsRet.paths;
            for (auto&& [newName, oldName] : modifiedPathsRet.renames) {
                // For the purposes of checking which paths are modified, consider renames to
                // preserve the path. We'll circle back later to figure out the new name if
                // appropriate. If we are going forward, we want to consider the name of the path
                // before 'stage'; otherwise, we want to consider the name as it exists after
                // 'stage'.
                auto preservedPath = (traversalDir == Direction::kForward) ? oldName : newName;
                preservedPaths.insert(preservedPath);
            }
            auto modifiedPaths = extractModifiedDependencies(pathsOfInterest, preservedPaths);
            if (modifiedPaths.empty()) {
                auto renameMap = (traversalDir == Direction::kForward)
                    ? invertRenameMap(modifiedPathsRet.renames)
                    : modifiedPathsRet.renames;
                return computeNamesAssumingAnyPathsNotRenamedAreUnmodified(renameMap,
                                                                           pathsOfInterest);
            }
            return boost::none;
        }
    }
    MONGO_UNREACHABLE;
}

boost::optional<StringMap<std::string>> renamedPaths(
    const Pipeline::SourceContainer::const_iterator start,
    const Pipeline::SourceContainer::const_iterator end,
    const OrderedPathSet& pathsOfInterest,
    boost::optional<std::function<bool(DocumentSource*)>> additionalStageValidatorCallback) {
    return renamedPathsFullPipeline(
        start, end, pathsOfInterest, Direction::kForward, additionalStageValidatorCallback);
}

boost::optional<StringMap<std::string>> renamedPaths(
    const Pipeline::SourceContainer::const_reverse_iterator start,
    const Pipeline::SourceContainer::const_reverse_iterator end,
    const OrderedPathSet& pathsOfInterest,
    boost::optional<std::function<bool(DocumentSource*)>> additionalStageValidatorCallback) {
    return renamedPathsFullPipeline(
        start, end, pathsOfInterest, Direction::kBackward, additionalStageValidatorCallback);
}

std::pair<Pipeline::SourceContainer::const_iterator, StringMap<std::string>>
findLongestViablePrefixPreservingPaths(
    const Pipeline::SourceContainer::const_iterator start,
    const Pipeline::SourceContainer::const_iterator end,
    const OrderedPathSet& pathsOfInterest,
    boost::optional<std::function<bool(DocumentSource*)>> additionalStageValidatorCallback) {
    return multiStageRenamedPaths(
        start, end, pathsOfInterest, Direction::kForward, additionalStageValidatorCallback);
}
}  // namespace mongo::semantic_analysis
