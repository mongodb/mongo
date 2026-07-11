// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/field_ref.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <set>
#include <string>
#include <vector>

namespace mongo {

struct IndexBounds;
struct IndexKeyEntry;
struct Interval;
struct MultikeyMetadataAccessStats;
class IndexCatalogEntry;
class OperationContext;

/**
 * Returns an exact set or super-set of the bounds required to fetch the multikey metadata keys
 * relevant to 'field'.
 */
std::vector<Interval> getMultikeyPathIndexIntervalsForField(FieldRef field);

/**
 * Returns the intersection of 'fields' and the set of multikey metadata paths stored in the
 * wildcard index. Statistics reporting index seeks and keys examined are written to 'stats'.
 */
std::set<FieldRef> getWildcardMultikeyPathSet(OperationContext* opCtx,
                                              const UUID& collectionUuid,
                                              const IndexCatalogEntry* entry,
                                              const stdx::unordered_set<std::string>& fieldSet,
                                              MultikeyMetadataAccessStats* stats);

}  // namespace mongo
