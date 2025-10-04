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

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <functional>
#include <set>
#include <string>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::semantic_analysis {

enum class Direction { kForward, kBackward };

/**
 * Action to take when `renamedPaths` finds that a path of interest has been modified.
 */
enum class UnpreservedPathPolicy {
    // Stop when _any_ field of interest has been modified (not by a simple rename), returning
    // boost::none.
    Fail,
    // Discard any modified fields from the set of interested paths.
    Discard
};

/**
 * Takes in a set of paths the caller is interested in, a pipeline stage, and a direction.  If the
 * direction is "forward", the paths given must exist before the stage is executed in the pipeline.
 * If the direction is "backward," the paths must exist after the stage is executed in the pipeline.
 * If all of the pathsOfInterest are preserved (but possibly renamed), we return a mapping from
 * [pathOfInterest] --> [newName], where "newName" is the name of "pathOfInterest" on the "other
 * side" of the stage with respect to the given direction.
 *
 * If any of the pathsOfInterest are modified by the current stage:
 *  * UnpreservedPathPolicy::Fail, returns boost::none
 *  * UnpreservedPathPolicy::Discard, returns a mapping, excluding the paths which have been
 *        modified (i.e., acts as if that field was not included in pathsOfInterest).
 *
 * For example, say pathsOfInterest contains "a", a path name that exists before nextStage is run in
 * the pipeline, and direction is forward. Say nextStage preserves all the paths but renames "a" to
 * "b"; we would return a mapping a-->b.
 *
 * Conversely, say pathsOfInterest contains "b", a path name that exists after nextStage is run in
 * the pipeline, and direction is backward. Say nextStage preserves all the paths but renamed "a" to
 * "b"; we would return a mapping b-->a.
 */
boost::optional<StringMap<std::string>> renamedPaths(
    const OrderedPathSet& pathsOfInterest,
    const DocumentSource& stage,
    const Direction& traversalDir,
    const UnpreservedPathPolicy& unpreservedPathPolicy = UnpreservedPathPolicy::Fail);
/**
 * Tracks renames by walking a pipeline forwards. Takes two forward iterators that represent two
 * stages in an aggregation pipeline, with 'start' coming before 'end,' as well as a set of path
 * names the caller is interested in. Returns boost::none if any of these paths are modified by the
 * pipeline from stages ['start' - 'end'); otherwise returns a mapping for each path in
 * 'pathsOfInterest' from its name before stage 'start' to its name before stage 'end'. To track
 * renames through an entire pipeline, 'start' should refer to the first stage in the pipeline while
 * 'end' should be an iterator referring to the past-the-end stage.
 */
boost::optional<StringMap<std::string>> renamedPaths(
    DocumentSourceContainer::const_iterator start,
    DocumentSourceContainer::const_iterator end,
    const OrderedPathSet& pathsOfInterest,
    boost::optional<std::function<bool(DocumentSource*)>> additionalStageValidatorCallback =
        boost::none);

/**
 * Tracks renames by walking a pipeline backwards. Takes two reverse iterators that represent two
 * stages in an aggregation pipeline, with 'start' coming after 'end,' as well as a set of path
 * names the caller is interested in. Returns boost::none if any of these paths were modified by the
 * pipeline from stages ('end' - 'start']; otherwise returns a mapping for each path in
 * 'pathsOfInterest' from its name after stage 'start' to its name as it was after stage 'end'. To
 * track renames through an entire pipeline, 'start' should refer to the last stage in the pipeline
 * while 'end' should refer to the (hypothetical) stage preceeding the first stage in the pipeline
 * (the 'reverse end').
 */
boost::optional<StringMap<std::string>> renamedPaths(
    DocumentSourceContainer::const_reverse_iterator start,
    DocumentSourceContainer::const_reverse_iterator end,
    const OrderedPathSet& pathsOfInterest,
    boost::optional<std::function<bool(DocumentSource*)>> additionalStageValidatorCallback =
        boost::none);

/**
 * Attempts to find a maximal prefix of the pipeline given by 'start' and 'end' which will preserve
 * all paths in 'pathsOfInterest' and also have each DocumentSource satisfy
 * 'additionalStageValidatorCallback'.
 *
 * Returns an iterator to the first stage which modifies one of the paths in 'pathsOfInterest' or
 * fails 'additionalStageValidatorCallback', or returns 'end' if no such stage exists.
 */
std::pair<DocumentSourceContainer::const_iterator, StringMap<std::string>>
findLongestViablePrefixPreservingPaths(DocumentSourceContainer::const_iterator start,
                                       DocumentSourceContainer::const_iterator end,
                                       const OrderedPathSet& pathsOfInterest,
                                       boost::optional<std::function<bool(DocumentSource*)>>
                                           additionalStageValidatorCallback = boost::none);

struct PartitionedDependencies {
    OrderedPathSet modified;
    OrderedPathSet preserved;
};
/**
 * Given a set of paths 'dependencies', determines which of those paths will be modified if all
 * paths except those in 'preservedPaths' are modified.
 *
 * For example, extractModifiedDependencies({'a', 'b', 'c.d', 'e'}, {'a', 'b.c', c'}) returns
 * {'b', 'e'}, since 'b' and 'e' are not preserved (only 'b.c' is preserved).
 */
PartitionedDependencies extractModifiedDependencies(const OrderedPathSet& dependencies,
                                                    const OrderedPathSet& preservedPaths);

bool pathSetContainsOverlappingPath(const OrderedPathSet& paths, const std::string& targetPath);

/**
 * Given a set of paths which exist at the end of the provided pipeline, find the
 * paths these existed as at the start of the pipeline.
 *
 * If any of the paths were added or overwritten by intermediate stages, the result will omit them.
 */
OrderedPathSet traceOriginatingPaths(const DocumentSourceContainer& pipeline,
                                     const OrderedPathSet& pathsOfInterest);

}  // namespace mongo::semantic_analysis
