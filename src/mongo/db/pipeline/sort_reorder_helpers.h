// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Checks if a sort stage's pattern is suitable to push the stage before $lookup or $graphLookup.
 * The sort stage must not share the same prefix with any field created or modified by the lookup
 * stage.
 */
bool checkModifiedPathsSortReorder(const SortPattern& sortPattern,
                                   const DocumentSource::GetModPathsReturn& modPaths);

/**
 * Tries to swap $lookup or $graphLookup with sort.
 */
DocumentSourceContainer::iterator tryReorderingWithSort(DocumentSourceContainer::iterator itr,
                                                        DocumentSourceContainer* container);

}  // namespace mongo
