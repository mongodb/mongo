/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/count_command_gen.h"
#include "mongo/db/query/find_command.h"

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
