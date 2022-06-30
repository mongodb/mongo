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

#include "mongo/db/commands/update_metrics.h"

namespace mongo {
UpdateMetrics::UpdateMetrics(StringData commandName)
    : _commandsWithAggregationPipeline("commands." + commandName + ".pipeline"),
      _commandsWithArrayFilters("commands." + commandName + ".arrayFilters") {}

void UpdateMetrics::incrementExecutedWithAggregationPipeline() {
    _commandsWithAggregationPipeline.increment();
}

void UpdateMetrics::incrementExecutedWithArrayFilters() {
    _commandsWithArrayFilters.increment();
}

void UpdateMetrics::collectMetrics(const BSONObj& cmdObj) {
    // If this command is a pipeline-style update, record that it was used.
    if (cmdObj.hasField("update") && (cmdObj.getField("update").type() == BSONType::Array)) {
        _commandsWithAggregationPipeline.increment();
    }

    // If this command had arrayFilters option, record that it was used.
    if (cmdObj.hasField("arrayFilters")) {
        _commandsWithArrayFilters.increment();
    }
}

void UpdateMetrics::collectMetrics(const write_ops::FindAndModifyCommandRequest& cmd) {
    if (auto update = cmd.getUpdate()) {
        if (update->type() == write_ops::UpdateModification::Type::kPipeline) {
            _commandsWithAggregationPipeline.increment();
        }
    }

    if (cmd.getArrayFilters()) {
        _commandsWithArrayFilters.increment();
    }
}

}  // namespace mongo
