// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {

class SearchQueryViewSpec;

namespace search_index_view_validation {

/**
 * Validates that the view's effective pipeline can be used with a search index. The restrictions
 * are as follows:
 *    - Only $addFields ($set) and $match can be used.
 *    - $addFields and cannot modify _id.
 *    - $match can be empty or contain only $expr.
 *    - Variables $$NOW, $$CLUSTER_TIME, and $$USER_ROLES cannot be used.
 *    - Operators $rand and $function cannot be used.
 *    - Overriding the CURRENT variable with $let is not allowed.
 */
void validate(const SearchQueryViewSpec& view);

}  // namespace search_index_view_validation

}  // namespace mongo
