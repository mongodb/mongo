// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Returns true if 'opName' is one of $first/$firstN/$last/$lastN.
 */
bool isPositionalAccumulator(std::string_view opName);

/**
 * Replaces AccumulationStatement 'stmt' with a custom accumulator for $bucketAuto.
 */
AccumulationStatement replaceAccumulationStatementForBucketAuto(ExpressionContext* expCtx,
                                                                AccumulationStatement&& stmt);

}  // namespace mongo
