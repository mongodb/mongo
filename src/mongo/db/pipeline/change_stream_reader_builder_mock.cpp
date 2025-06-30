/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/change_stream_reader_builder_mock.h"

namespace mongo {

//
// Static method stubs for usage in tests
//

// Returns a nullptr shard targeter.
const ChangeStreamReaderBuilderMock::BuildShardTargeterFunction
    ChangeStreamReaderBuilderMock::emptyShardTargeter = [](OperationContext*, const ChangeStream&) {
        return std::unique_ptr<ChangeStreamShardTargeter>();
    };

// Returns an empty BSONObj control events filter.
const ChangeStreamReaderBuilderMock::BuildControlEventFunction
    ChangeStreamReaderBuilderMock::emptyControlEvents = [](OperationContext*, const ChangeStream&) {
        return BSONObj();
    };

// Returns an empty set of control events.
const ChangeStreamReaderBuilderMock::GetControlEventTypesFunction
    ChangeStreamReaderBuilderMock::emptyControlEventTypes =
        [](OperationContext*, const ChangeStream&) {
            return std::set<std::string>{};
        };

ChangeStreamReaderBuilderMock::ChangeStreamReaderBuilderMock(
    BuildShardTargeterFunction buildShardTargeter,
    BuildControlEventFunction controlEventFilterForDataShard,
    GetControlEventTypesFunction controlEventTypesOnDataShard,
    BuildControlEventFunction controlEventFilterForConfigServer,
    GetControlEventTypesFunction controlEventTypesOnConfigServer)
    : _buildShardTargeter(std::move(buildShardTargeter)),
      _controlEventFilterForDataShard(std::move(controlEventFilterForDataShard)),
      _controlEventTypesOnDataShard(std::move(controlEventTypesOnDataShard)),
      _controlEventFilterForConfigServer(std::move(controlEventFilterForConfigServer)),
      _controlEventTypesOnConfigServer(std::move(controlEventTypesOnConfigServer)) {}

std::unique_ptr<ChangeStreamShardTargeter> ChangeStreamReaderBuilderMock::buildShardTargeter(
    OperationContext* opCtx, const ChangeStream& changeStream) {
    return _buildShardTargeter(opCtx, changeStream);
}

BSONObj ChangeStreamReaderBuilderMock::buildControlEventFilterForDataShard(
    OperationContext* opCtx, const ChangeStream& changeStream) {
    return _controlEventFilterForDataShard(opCtx, changeStream);
}

std::set<std::string> ChangeStreamReaderBuilderMock::getControlEventTypesOnDataShard(
    OperationContext* opCtx, const ChangeStream& changeStream) {
    return _controlEventTypesOnDataShard(opCtx, changeStream);
}

BSONObj ChangeStreamReaderBuilderMock::buildControlEventFilterForConfigServer(
    OperationContext* opCtx, const ChangeStream& changeStream) {
    return _controlEventFilterForConfigServer(opCtx, changeStream);
}

std::set<std::string> ChangeStreamReaderBuilderMock::getControlEventTypesOnConfigServer(
    OperationContext* opCtx, const ChangeStream& changeStream) {
    return _controlEventTypesOnConfigServer(opCtx, changeStream);
}

}  // namespace mongo
