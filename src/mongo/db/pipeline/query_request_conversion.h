// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/count_command_gen.h"
#include "mongo/db/query/find_command.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Contains utilities for converting one kind of request to another.
 */
namespace query_request_conversion {

/**
 * Converts this FindCommandRequest into the corresponding AggregationCommandRequest.
 *
 * If this FindCommandRequest has options that cannot be satisfied by aggregation, throws a user
 * assertion with ErrorCodes::InvalidPipelineOperator.
 */
AggregateCommandRequest asAggregateCommandRequest(const FindCommandRequest& findCommand,
                                                  bool hasExplain = false);

/**
 * Converts this CountCommandRequest into the corresponding AggregationCommandRequest.
 */
AggregateCommandRequest asAggregateCommandRequest(const CountCommandRequest& countCommand,
                                                  bool hasExplain = false);

}  // namespace query_request_conversion
}  // namespace mongo
