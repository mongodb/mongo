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

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/change_stream_reader_builder.h"

#include <functional>
#include <memory>
#include <set>
#include <string>

namespace mongo {

class ChangeStreamReaderBuilderMock : public ChangeStreamReaderBuilder {
public:
    // Convenience type aliases.
    using BuildShardTargeterFunction = std::function<std::unique_ptr<ChangeStreamShardTargeter>(
        OperationContext* opCtx, const ChangeStream& changeStream)>;
    using BuildControlEventFunction =
        std::function<BSONObj(OperationContext* opCtx, const ChangeStream& changeStream)>;
    using GetControlEventTypesFunction = std::function<std::set<std::string>(
        OperationContext* opCtx, const ChangeStream& changeStream)>;

    /**
     * Create a 'ChangeStreamReaderBuilder' mock object.
     * By default, all functions are mocked to return nothing (i.e. a nullptr shard targeter, empty
     * filter BSONObjs and empty sets of control events). The individual mock functions can be
     * overridden as necessary.
     */
    ChangeStreamReaderBuilderMock(
        BuildShardTargeterFunction buildShardTargeter =
            [](OperationContext*, const ChangeStream&) {
                return std::unique_ptr<ChangeStreamShardTargeter>();
            },
        BuildControlEventFunction controlEventFilterForDataShard =
            [](OperationContext*, const ChangeStream&) { return BSONObj(); },
        GetControlEventTypesFunction controlEventTypesOnDataShard =
            [](OperationContext*, const ChangeStream&) { return std::set<std::string>{}; },
        BuildControlEventFunction controlEventFilterForConfigServer =
            [](OperationContext*, const ChangeStream&) { return BSONObj(); },
        GetControlEventTypesFunction controlEventTypesOnConfigServer =
            [](OperationContext*, const ChangeStream&) { return std::set<std::string>{}; })
        : _buildShardTargeter(std::move(buildShardTargeter)),
          _controlEventFilterForDataShard(std::move(controlEventFilterForDataShard)),
          _controlEventTypesOnDataShard(std::move(controlEventTypesOnDataShard)),
          _controlEventFilterForConfigServer(std::move(controlEventFilterForConfigServer)),
          _controlEventTypesOnConfigServer(std::move(controlEventTypesOnConfigServer)) {}

    /**
     * Invokes '_buildShardTargeterFunction' with the specified parameters.
     */
    std::unique_ptr<ChangeStreamShardTargeter> buildShardTargeter(
        OperationContext* opCtx, const ChangeStream& changeStream) override {
        return _buildShardTargeter(opCtx, changeStream);
    }

    /**
     * Invokes '_buildControlEventFilterForDataShard' with the specified parameters.
     */
    BSONObj buildControlEventFilterForDataShard(OperationContext* opCtx,
                                                const ChangeStream& changeStream) override {
        return _controlEventFilterForDataShard(opCtx, changeStream);
    }

    /**
     * Invokes '_getControlEventTypesOnDataShard' with the specified parameters.
     */
    std::set<std::string> getControlEventTypesOnDataShard(
        OperationContext* opCtx, const ChangeStream& changeStream) override {
        return _controlEventTypesOnDataShard(opCtx, changeStream);
    }

    /**
     * Invokes '_buildControlEventFilterForConfigServer' with the specified parameters.
     */
    BSONObj buildControlEventFilterForConfigServer(OperationContext* opCtx,
                                                   const ChangeStream& changeStream) override {
        return _controlEventFilterForConfigServer(opCtx, changeStream);
    }

    /**
     * Invokes '_getControlEventTypesOnConfigServer' with the specified parameters.
     */
    std::set<std::string> getControlEventTypesOnConfigServer(
        OperationContext* opCtx, const ChangeStream& changeStream) override {
        return _controlEventTypesOnConfigServer(opCtx, changeStream);
    }

private:
    BuildShardTargeterFunction _buildShardTargeter;
    BuildControlEventFunction _controlEventFilterForDataShard;
    GetControlEventTypesFunction _controlEventTypesOnDataShard;
    BuildControlEventFunction _controlEventFilterForConfigServer;
    GetControlEventTypesFunction _controlEventTypesOnConfigServer;
};

}  // namespace mongo
