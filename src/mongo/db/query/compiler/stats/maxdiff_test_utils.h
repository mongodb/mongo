// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/stats/value_utils.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string>
#include <vector>

namespace mongo::stats {

/**
    Given a list of SBE values and a query, create a collection containing the data,
    and count the results from the supplied query.
 */
size_t getActualCard(OperationContext* opCtx,
                     const std::vector<SBEValue>& input,
                     const std::string& query);

/**
    Serialize a vector of values.
*/
std::string printValueArray(const std::vector<SBEValue>& values);
}  // namespace mongo::stats
