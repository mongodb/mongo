// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/modules.h"

namespace mongo::stats {

using TypeTagPairs = std::vector<std::pair<sbe::value::TypeTags, sbe::value::TypeTags>>;

/**
 * An alternative implementation of sameTypeClass by comparing the minimum values of two TypeTags
 * with BSONObjBuilder::appendMinForType.
 */
bool sameTypeClassByComparingMin(sbe::value::TypeTags tag1, sbe::value::TypeTags tag2);

/**
 * Generates all possible pairs of TypeTags for testing.
 */
TypeTagPairs generateAllTypeTagPairs();

/**
 * Generates all possible pairs of shallow TypeTags for testing.
 */
TypeTagPairs generateShallowTypeTagPairs();

/**
 * Generates all possible pairs of heap-based TypeTags for testing.
 */
TypeTagPairs generateHeapTypeTagPairs();

}  // namespace mongo::stats
