// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/change_stream_reader_builder.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

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
        auto shardTargeter = _buildShardTargeter(opCtx, changeStream);

        // Keep a pointer to the constructed shard targeter, so it can be retrieved via
        // 'getShardTargeter()' later.
        _shardTargeter = shardTargeter.get();
        return shardTargeter;
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

    /**
     * Returns a pointer to the constructed shard targeter, if any. May return a nullptr.
     */
    ChangeStreamShardTargeter* getShardTargeter() const {
        return _shardTargeter;
    }

private:
    BuildShardTargeterFunction _buildShardTargeter;
    BuildControlEventFunction _controlEventFilterForDataShard;
    GetControlEventTypesFunction _controlEventTypesOnDataShard;
    BuildControlEventFunction _controlEventFilterForConfigServer;
    GetControlEventTypesFunction _controlEventTypesOnConfigServer;

    ChangeStreamShardTargeter* _shardTargeter = nullptr;
};

/**
 * RAII scopeguard for installing a 'ChangeStreamReaderBuilder' mock in the global service context.
 * On construction, the previously-installed decoration (if any) is saved aside; on destruction,
 * that previous decoration is restored. This makes the guard safely nestable — fixtures can
 * register a baseline mock and individual tests can stack their own on top.
 */
struct ScopedChangeStreamReaderBuilderMock {
    ScopedChangeStreamReaderBuilderMock(const ScopedChangeStreamReaderBuilderMock&) = delete;
    ScopedChangeStreamReaderBuilderMock& operator=(const ScopedChangeStreamReaderBuilderMock&) =
        delete;

    explicit ScopedChangeStreamReaderBuilderMock(
        std::unique_ptr<ChangeStreamReaderBuilder> readerBuilder) {
        _previous = ChangeStreamReaderBuilder::swap_forTest(getGlobalServiceContext(),
                                                            std::move(readerBuilder));
    }

    ~ScopedChangeStreamReaderBuilderMock() {
        ChangeStreamReaderBuilder::swap_forTest(getGlobalServiceContext(), std::move(_previous));
    }

private:
    std::unique_ptr<ChangeStreamReaderBuilder> _previous;
};

}  // namespace mongo
