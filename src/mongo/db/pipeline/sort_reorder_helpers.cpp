// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/sort_reorder_helpers.h"

#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <algorithm>
#include <iterator>
#include <list>
#include <map>
#include <utility>
#include <vector>


namespace mongo {

bool checkModifiedPathsSortReorder(const SortPattern& sortPattern,
                                   const DocumentSource::GetModPathsReturn& modPaths) {
    for (const auto& sortKey : sortPattern) {
        if (!sortKey.fieldPath.has_value()) {
            return false;
        }
        if (sortKey.fieldPath->getPathLength() < 1) {
            return false;
        }
        auto sortField = sortKey.fieldPath->getFieldName(0);
        auto it = std::find_if(
            modPaths.paths.begin(), modPaths.paths.end(), [&sortField](const auto& modPath) {
                // Finds if the shorter path is a prefix field of or the same as the longer one.
                return sortField == modPath || expression::isPathPrefixOf(sortField, modPath) ||
                    expression::isPathPrefixOf(modPath, sortField);
            });
        if (it != modPaths.paths.end()) {
            return false;
        }
    }
    return true;
}

DocumentSourceContainer::iterator tryReorderingWithSort(DocumentSourceContainer::iterator itr,
                                                        DocumentSourceContainer* container) {
    auto docSource = itr->get();
    tassert(11282910,
            "Reordering with sort only works with $lookup and $graphLookup",
            docSource->isInstanceOf<DocumentSourceLookUp>() ||
                docSource->isInstanceOf<DocumentSourceGraphLookUp>());

    // If we have $graphLookup or $lookup followed by $sort, and $sort does not use any fields
    // created by it, they can swap.
    // TODO (SERVER-55417): Conditionally reorder $sort and $lookup depending on whether the
    // query planner allows for an index-provided sort.
    auto nextSort = dynamic_cast<DocumentSourceSort*>(std::next(itr)->get());
    if (nextSort &&
        checkModifiedPathsSortReorder(nextSort->getSortPattern(), docSource->getModifiedPaths())) {
        std::swap(*itr, *std::next(itr));
        return itr == container->begin() ? itr : std::prev(itr);
    }

    return itr;
}

}  // namespace mongo
