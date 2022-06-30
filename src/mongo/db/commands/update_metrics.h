/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/ops/write_ops.h"

namespace mongo {
/**
 * Execution metrics for update type commands - update, findAndModify and their mongoS variants.
 * Exposes the metrics to the output of the serverStatus command.
 */
class UpdateMetrics {
public:
    /**
     * Construct metrics for a command identified by commandName.
     */
    UpdateMetrics(StringData commandName);

    /**
     * Increment counter for how many times this command has executed with an aggregation
     * pipeline-style update parameter.
     */
    void incrementExecutedWithAggregationPipeline();

    /**
     * Increment counter for how many times this command has executed with the arrayFilters option.
     */
    void incrementExecutedWithArrayFilters();

    /**
     * Collect update related metrics from the command object for update type commands. Update type
     * commands may have update and arrayFilters fields. Accepts an unparsed command object to
     * support use cases when the command object is not fully parsed by the command.
     */
    void collectMetrics(const BSONObj& cmdObj);

    /**
     * Increments update metrics corresponding to the supplied parameters.
     */
    void collectMetrics(const write_ops::FindAndModifyCommandRequest& cmd);

private:
    // A counter for how many times this command has been executed with an aggregation
    // pipeline-style update parameter.
    CounterMetric _commandsWithAggregationPipeline;

    // A counter for how many times this command has been executed with the arrayFilters option.
    CounterMetric _commandsWithArrayFilters;
};
}  // namespace mongo
