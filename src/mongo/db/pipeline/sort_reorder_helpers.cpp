/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/pipeline/sort_reorder_helpers.h"

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

Pipeline::SourceContainer::iterator tryReorderingWithSort(Pipeline::SourceContainer::iterator itr,
                                                          Pipeline::SourceContainer* container) {
    auto docSource = itr->get();
    invariant(dynamic_cast<DocumentSourceLookUp*>(docSource) ||
              dynamic_cast<DocumentSourceGraphLookUp*>(docSource));

    // If we have $graphLookup or $lookup followed by $sort, and $sort does not use any fields
    // created by it, they can swap.
    // TODO (SERVER-55417): Conditionally reorder $sort and $lookup depending on whether the
    // query planner allows for an index-provided sort.
    auto nextSort = dynamic_cast<DocumentSourceSort*>(std::next(itr)->get());
    if (nextSort &&
        checkModifiedPathsSortReorder(nextSort->getSortKeyPattern(),
                                      docSource->getModifiedPaths())) {
        std::swap(*itr, *std::next(itr));
        return itr == container->begin() ? itr : std::prev(itr);
    }

    return itr;
}

}  // namespace mongo
