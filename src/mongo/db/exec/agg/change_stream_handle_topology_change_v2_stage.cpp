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

#include "mongo/db/exec/agg/change_stream_handle_topology_change_v2_stage.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/change_stream_pipeline_helpers.h"
#include "mongo/db/pipeline/change_stream_read_mode.h"
#include "mongo/db/pipeline/change_stream_reader_builder.h"
#include "mongo/db/pipeline/change_stream_reader_context.h"
#include "mongo/db/pipeline/change_stream_shard_targeter.h"
#include "mongo/db/pipeline/change_stream_topology_helpers.h"
#include "mongo/db/pipeline/data_to_shards_allocation_query_service.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change_v2.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/redaction.h"
#include "mongo/s/change_streams/change_stream_reader_builder_impl.h"
#include "mongo/s/query/exec/establish_cursors.h"
#include "mongo/s/query/exec/shard_tag.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <algorithm>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

// Prefix that will be added to all log messages emitted by this stage.
#define STAGE_LOG_PREFIX "ChangeStreamHandleTopologyChangeV2Stage: "

namespace mongo {
namespace {

/**
 * Return the correct namespace for pipelines that are built on the config server. This is always
 * the "admin" namespace.
 */
NamespaceString buildNamespaceForConfigServerPipeline() {
    return NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin);
}

/**
 * A cursor manager implementation that uses the preceding 'MergeCursors' stage in the pipeline
 * to open and close cursors.
 */
class V2StageCursorManager final
    : public exec::agg::ChangeStreamHandleTopologyChangeV2Stage::CursorManager {
public:
    V2StageCursorManager(const ChangeStream& changeStream, ChangeStreamReaderBuilder* readerBuilder)
        : _changeStream(changeStream), _readerBuilder(readerBuilder) {}

    void initialize(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                    exec::agg::ChangeStreamHandleTopologyChangeV2Stage* stage,
                    const ResumeTokenData& resumeTokenData) override {
        _mergeCursors = stage->getSourceStage();

        _mergeCursors->recognizeControlEvents();

        _initializationResumeToken = ResumeToken(resumeTokenData);
        _mergeCursors->setInitialHighWaterMark(_initializationResumeToken.toBSON());

        _originalAggregateCommand = expCtx->getOriginalAggregateCommand().getOwned();
    }

    void openCursorsOnDataShards(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 OperationContext* opCtx,
                                 Timestamp atClusterTime,
                                 const stdx::unordered_set<ShardId>& shardIds) override {
        auto openedCursors =
            openCursors(opCtx, atClusterTime, shardIds, false /* isConfigServer */, [&]() {
                // Build the change stream pipeline command to be run on the data shards.
                // '_originalAggregateCommand' already contains the relevant match expressions
                // for the oplog.
                auto cmdObj = [&]() {
                    // If the 'atClusterTime' matches the clusterTime of
                    // '_initializationResumeToken', we should open the $changeStream cursors by
                    // passing the original resume token.
                    const bool isInitialRequest =
                        atClusterTime == _initializationResumeToken.getClusterTime();
                    if (isInitialRequest) {
                        return change_stream::topology_helpers::createUpdatedCommandForNewShard(
                            expCtx,
                            _initializationResumeToken,
                            _originalAggregateCommand,
                            ChangeStreamReaderVersionEnum::kV2);
                    }

                    return change_stream::topology_helpers::createUpdatedCommandForNewShard(
                        expCtx,
                        atClusterTime,
                        _originalAggregateCommand,
                        ChangeStreamReaderVersionEnum::kV2);
                }();

                LOGV2_DEBUG(10657554,
                            3,
                            STAGE_LOG_PREFIX "Built pipeline command for data shard",
                            "cmdObj"_attr = redact(cmdObj),
                            "changeStream"_attr = _changeStream);

                std::vector<AsyncRequestsSender::Request> remotes;
                remotes.reserve(shardIds.size());
                for (auto&& shardId : shardIds) {
                    remotes.emplace_back(shardId, cmdObj);
                }

                return establishCursors(opCtx,
                                        expCtx->getMongoProcessInterface()->taskExecutor,
                                        expCtx->getNamespaceString(),
                                        ReadPreferenceSetting::get(opCtx),
                                        remotes,
                                        false /* allowPartialResults */);
            });

        _mergeCursors->addNewShardCursors(std::move(openedCursors), ShardTag::kDataShard);

        _currentlyTargetedDataShards.insert(shardIds.begin(), shardIds.end());
    }

    void openCursorOnConfigServer(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  OperationContext* opCtx,
                                  Timestamp atClusterTime) override {
        const auto shardId = getConfigShardId(opCtx);
        auto openedCursors =
            openCursors(opCtx, atClusterTime, {shardId}, true /* isConfigServer */, [&]() {
                // The change stream on the config server will be opened as a change stream on the
                // "admin" namespace.
                const NamespaceString nss = buildNamespaceForConfigServerPipeline();
                tassert(10657546,
                        "expecting adminDB namespace for config server change stream",
                        nss.isAdminDB());

                // Build the V2 change stream pipeline command to be run on the config server.
                std::vector<BSONObj> serializedPipeline =
                    change_stream::pipeline_helpers::buildPipelineForConfigServerV2(
                        expCtx, opCtx, atClusterTime, nss, _changeStream, _readerBuilder);

                LOGV2_DEBUG(10657551,
                            3,
                            STAGE_LOG_PREFIX "Built pipeline for config server",
                            "pipeline"_attr = serializedPipeline,
                            "changeStream"_attr = _changeStream);

                AggregateCommandRequest aggReq(nss, std::move(serializedPipeline));

                aggregation_request_helper::setFromRouter(
                    VersionContext::getDecoration(opCtx), aggReq, true);
                aggReq.setNeedsMerge(true);

                SimpleCursorOptions cursor;
                cursor.setBatchSize(0);
                aggReq.setCursor(cursor);
                setReadWriteConcern(opCtx, aggReq, true, !expCtx->getExplain());

                return establishCursors(opCtx,
                                        expCtx->getMongoProcessInterface()->taskExecutor,
                                        aggReq.getNamespace(),
                                        ReadPreferenceSetting{ReadPreference::SecondaryPreferred},
                                        {{shardId, aggReq.toBSON()}},
                                        false /* allowPartialResults */);
            });

        _mergeCursors->addNewShardCursors(std::move(openedCursors), ShardTag::kConfigServer);
    }

    void closeCursorsOnDataShards(const stdx::unordered_set<ShardId>& shardIds) override {
        closeCursors(shardIds, false /* isConfigServer */);

        for (const auto& shardId : shardIds) {
            _currentlyTargetedDataShards.erase(shardId);
        }
    }

    void closeCursorOnConfigServer(OperationContext* opCtx) override {
        closeCursors({getConfigShardId(opCtx)}, true /* isConfigServer */);
    }

    const stdx::unordered_set<ShardId>& getCurrentlyTargetedDataShards() const override {
        return _currentlyTargetedDataShards;
    }

    const ChangeStream& getChangeStream() const override {
        return _changeStream;
    }

    void enableUndoNextMode() override {
        _mergeCursors->enableUndoNextMode();
    }

    void disableUndoNextMode() override {
        _mergeCursors->disableUndoNextMode();
    }

    void undoGetNextAndSetHighWaterMark(Timestamp highWaterMark) override {
        _mergeCursors->undoNext();
        _mergeCursors->setHighWaterMark(ResumeToken::makeHighWaterMarkToken(
                                            highWaterMark, ResumeTokenData::kDefaultTokenVersion)
                                            .toDocument()
                                            .toBson());
    }

    Timestamp getTimestampFromCurrentHighWaterMark() const override {
        BSONObj highWaterMark = _mergeCursors->getHighWaterMark();
        tassert(10657533,
                "Expected high-water mark to be an object",
                highWaterMark.firstElement().type() == BSONType::object);
        return ResumeToken::parse(highWaterMark.firstElement().Obj()).getData().clusterTime;
    }

private:
    /**
     * Return the shard id of the config server.
     */
    ShardId getConfigShardId(OperationContext* opCtx) const {
        return Grid::get(opCtx)->shardRegistry()->getConfigShard()->getId();
    }

    /**
     * Reload the shard registry if at least one of specified shard ids is missing from it.
     */
    void reloadShardRegistryIfNotAllShardsArePresent(OperationContext* opCtx,
                                                     const stdx::unordered_set<ShardId>& shardIds) {
        bool allRequestedShardsArePresent = [&]() {
            auto allAvailableShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
            std::sort(allAvailableShardIds.begin(), allAvailableShardIds.end());
            return std::all_of(shardIds.begin(), shardIds.end(), [&](const auto& shardId) {
                return std::binary_search(
                    allAvailableShardIds.begin(), allAvailableShardIds.end(), shardId);
            });
        }();

        if (!allRequestedShardsArePresent) {
            // In case one of the target shards is not known by the shard registry, reload the shard
            // registry proactively.
            reloadShardRegistry(opCtx, shardIds);
        }
    }

    void reloadShardRegistry(OperationContext* opCtx,
                             const stdx::unordered_set<ShardId>& shardIds) {
        LOGV2_DEBUG(
            10657552,
            3,
            STAGE_LOG_PREFIX
            "Did not find all requested shardIds in shard registry - refreshing shard registry",
            "shardIds"_attr = shardIds,
            "changeStream"_attr = _changeStream);

        Grid::get(opCtx)->shardRegistry()->reload(opCtx);
    }

    /**
     * Helper function to open cursors, called by both 'openCursorsOnDataShards()' and
     * 'openCursorOnConfigServer()'.
     */
    std::vector<RemoteCursor> openCursors(
        OperationContext* opCtx,
        Timestamp atClusterTime,
        const stdx::unordered_set<ShardId>& shardIds,
        bool isConfigServer,
        const std::function<std::vector<RemoteCursor>()>& callback) {
        tassert(10657516, "expected _mergeCursors to be set", _mergeCursors != nullptr);
        tassert(
            10657524, "expecting at least one shard id when opening cursors", !shardIds.empty());
        tassert(10657525,
                str::stream() << "expecting exactly one shard id for the config server, but got "
                              << shardIds.size(),
                !isConfigServer || shardIds.size() == 1);

        LOGV2_DEBUG(10657531,
                    3,
                    STAGE_LOG_PREFIX "Trying to establish cursors on shards",
                    "shardIds"_attr = shardIds,
                    "isConfigServer"_attr = isConfigServer,
                    "atClusterTime"_attr = atClusterTime,
                    "changeStream"_attr = _changeStream);

        // Reload the shard registry in case the requested shardIds is not present in the shard set
        // of the local shard registry copy.
        reloadShardRegistryIfNotAllShardsArePresent(opCtx, shardIds);

        try {
            // Run the callback function to open the cursors. This may throw an exception.
            return callback();
        } catch (const DBException& ex) {
            LOGV2_DEBUG(10657545,
                        3,
                        STAGE_LOG_PREFIX "Could not open cursors to all requested shards",
                        "shardIds"_attr = shardIds,
                        "error"_attr = redact(ex.toStatus()),
                        "changeStream"_attr = _changeStream);

            // Reload shard registry and validate that all requested shard ids are actually
            // contained in it.
            reloadShardRegistry(opCtx, shardIds);

            // If any of the requested shard ids cannot be found in the updated copy of the shard
            // registry, return a 'ShardNotFound' error from here, despite the type of exception we
            // originally caught. 'ShardNotFound' errors are "preferred" here, as they are handled
            // in a special way by the callers inside this stage, i.e. they get converted into fatal
            // 'ShardRemovedError's in strict more, and retryable 'RetryChangeStream' errors in
            // ignore-removed-shards mode.
            for (const ShardId& shardId : shardIds) {
                if (auto swRes = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId);
                    swRes.getStatus().code() == ErrorCodes::ShardNotFound) {
                    // If any shard id is indeed not found, we will get a 'ShardNotFound' error from
                    // the shard registry. In this case throw this error to the caller.
                    LOGV2_DEBUG(10657553,
                                3,
                                STAGE_LOG_PREFIX "Could not find shard in shard registry",
                                "shardId"_attr = shardId,
                                "error"_attr = redact(ex.toStatus()),
                                "shardStatus"_attr = redact(swRes.getStatus()),
                                "changeStream"_attr = _changeStream);

                    error_details::throwExceptionForStatus(swRes.getStatus());
                }
            }

            // In case all shard ids are present in the updated shard registry, rethrow the original
            // exception. This can be a 'ShardNotFound' exception or any other type of exception.
            throw;
        }
    }

    /**
     * Helper function to close cursors, called by both 'closeCursorsOnDataShards()' and
     * 'closeCursorOnConfigServer()'.
     */
    void closeCursors(const stdx::unordered_set<ShardId>& shardIds, bool isConfigServer) {
        tassert(10657517, "expected _mergeCursors to be set", _mergeCursors != nullptr);
        tassert(
            10657526, "expecting at least one shard id when closing cursors", !shardIds.empty());
        tassert(10657527,
                str::stream() << "expecting exactly one shard id for the config server, but got "
                              << shardIds.size(),
                !isConfigServer || shardIds.size() == 1);

        LOGV2_DEBUG(10657547,
                    3,
                    STAGE_LOG_PREFIX "Closing cursors on shards",
                    "shardIds"_attr = shardIds,
                    "isConfigServer"_attr = isConfigServer,
                    "changeStream"_attr = _changeStream);

        _mergeCursors->closeShardCursors(
            shardIds, isConfigServer ? ShardTag::kConfigServer : ShardTag::kDataShard);
    }

    // The underlying change stream used by the cursor manager.
    const ChangeStream _changeStream;

    // The reader builder is used to build the oplog match expression when opening the change
    // stream on the config server.
    ChangeStreamReaderBuilder* _readerBuilder;

    // The original aggregate pipeline command used when opening the change stream. Used when
    // opening change streams on data shards.
    BSONObj _originalAggregateCommand;

    // ResumeToken used for initializing the CursorManager.
    ResumeToken _initializationResumeToken;

    // Pointer to the preceding 'MergeCursors' stage. Will be set in 'initialize()'.
    exec::agg::MergeCursorsStage* _mergeCursors = nullptr;

    // The currently targeted data shards. This does not include the config server.
    stdx::unordered_set<ShardId> _currentlyTargetedDataShards;
};

// A ChangeStreamReaderContext implementation used by this stage
class V2StageReaderContext final : public ChangeStreamReaderContext {
    V2StageReaderContext(const V2StageReaderContext&) = delete;
    V2StageReaderContext& operator=(const V2StageReaderContext&) = delete;

public:
    V2StageReaderContext(
        boost::intrusive_ptr<ExpressionContext> expCtx,
        OperationContext* opCtx,
        exec::agg::ChangeStreamHandleTopologyChangeV2Stage::CursorManager& cursorManager,
        bool degradedMode)
        : _expCtx(expCtx),
          _opCtx(opCtx),
          _cursorManager(cursorManager),
          _degradedMode(degradedMode) {}

    /**
     * Record the request to open cursors on specific data shards.
     *
     * Preconditions for calling this method:
     * - no previous call to "openCursorsOnDataShards()" has been made in the same context.
     * - none of the shards in the shard set were included in a previous call to
     * "closeCursorsOnDataShards()" in the same context.
     */
    void openCursorsOnDataShards(Timestamp atClusterTime,
                                 const stdx::unordered_set<ShardId>& shardSet) override {
        if (!shardSet.empty()) {
            _bufferedRequests.validateStateForDataShardsAction(shardSet, true /* isOpen */);
            _bufferedRequests.openCursorsOnDataShards.emplace(
                std::make_pair(atClusterTime, shardSet));
        }
    }

    /**
     * Record the request to close cursors on specific data shards.
     *
     * Preconditions for calling this method:
     * - no previous call to "closeCursorsOnDataShards()" has been made in the same context.
     * - none of the shards in the shard set were included in a previous call to
     * "openCursorsOnDataShards()" in the same context.
     */
    void closeCursorsOnDataShards(const stdx::unordered_set<ShardId>& shardSet) override {
        if (!shardSet.empty()) {
            _bufferedRequests.validateStateForDataShardsAction(shardSet, false /* isOpen */);
            _bufferedRequests.closeCursorsOnDataShards.emplace(shardSet);
        }
    }

    /**
     * Record the request to open the cursor on the config server.
     *
     * Preconditions for calling this method:
     * - no previous call to "closeCursorOnConfigServer()" has been made in the same context.
     * - no previous call to "openCursorOnConfigServer()" has been made in the same context.
     */
    void openCursorOnConfigServer(Timestamp atClusterTime) override {
        _bufferedRequests.validateStateForConfigServerAction();
        _bufferedRequests.openCursorOnConfigServer.emplace(atClusterTime);
    }

    /**
     * Record the request to close the cursor on the config server.
     *
     * Preconditions for calling this method:
     * - no previous call to "closeCursorOnConfigServer()" has been made in the same context.
     * - no previous call to "openCursorOnConfigServer()" has been made in the same context.
     */
    void closeCursorOnConfigServer() override {
        _bufferedRequests.validateStateForConfigServerAction();
        _bufferedRequests.closeCursorOnConfigServer = true;
    }

    const stdx::unordered_set<ShardId>& getCurrentlyTargetedDataShards() const override {
        return _cursorManager.getCurrentlyTargetedDataShards();
    }

    const ChangeStream& getChangeStream() const override {
        return _cursorManager.getChangeStream();
    }

    bool inDegradedMode() const override {
        return _degradedMode;
    }

    /**
     * Whether or not any requests were buffered to execute later.
     */
    bool hasBufferedCursorRequests() const {
        return !_bufferedRequests.empty();
    }

    /**
     * Executes the opening and closing of cursors for all batches cursor open/close calls.
     * First opens additional cursors, then closes obsolete cursors.
     * The rationale for this is to always keep at least one cursor open at any time, because the
     * 'AsyncResultsMerger' relies on remotes being present.
     *
     * If an exception escapes during the execution of this method, the cursor open/close requests
     * executed by the method are not rolled back.
     */
    void executeCursorRequests() {
        auto& requests = _bufferedRequests;

        // Execute buffered requests to open cursors.
        if (requests.openCursorsOnDataShards.has_value()) {
            _cursorManager.openCursorsOnDataShards(_expCtx,
                                                   _opCtx,
                                                   requests.openCursorsOnDataShards->first,
                                                   requests.openCursorsOnDataShards->second);
        }
        if (requests.openCursorOnConfigServer.has_value()) {
            _cursorManager.openCursorOnConfigServer(
                _expCtx, _opCtx, *requests.openCursorOnConfigServer);
        }

        // Execute buffered requests to close cursors.
        if (requests.closeCursorsOnDataShards.has_value()) {
            _cursorManager.closeCursorsOnDataShards(*requests.closeCursorsOnDataShards);
        }
        if (requests.closeCursorOnConfigServer) {
            _cursorManager.closeCursorOnConfigServer(_opCtx);
        }
    }

private:
    // This struct keeps track of which cursor open/close calls have been recorded in the context.
    // The buffered requests will be executed in one go in the 'executeCursorRequests()' method of
    // the context.
    struct CursorRequests {
        boost::optional<std::pair<Timestamp, stdx::unordered_set<ShardId>>> openCursorsOnDataShards;
        boost::optional<Timestamp> openCursorOnConfigServer;
        boost::optional<stdx::unordered_set<ShardId>> closeCursorsOnDataShards;
        bool closeCursorOnConfigServer = false;

        void validateStateForDataShardsAction(const stdx::unordered_set<ShardId>& shardSet,
                                              bool isOpen) const {
            if (isOpen) {
                tassert(10657534,
                        "expecting open cursor request for data shards to be empty",
                        !openCursorsOnDataShards.has_value());

                for (auto&& shardId : shardSet) {
                    tassert(10657535,
                            "expecting no cursor close request for data shard to be present",
                            !closeCursorsOnDataShards.has_value() ||
                                !closeCursorsOnDataShards->contains(shardId));
                }
            } else {
                tassert(10657536,
                        "expecting close cursor request for data shards to be empty",
                        !closeCursorsOnDataShards.has_value());

                for (auto&& shardId : shardSet) {
                    tassert(10657537,
                            "expecting no cursor open request for data shard to be present",
                            !openCursorsOnDataShards.has_value() ||
                                !openCursorsOnDataShards->second.contains(shardId));
                }
            }
        }

        void validateStateForConfigServerAction() const {
            tassert(10657538,
                    "expecting open cursor request for the config server to be missing",
                    !openCursorOnConfigServer.has_value());
            tassert(10657539,
                    "expecting close cursor request for the config server to be missing",
                    !closeCursorOnConfigServer);
        }

        bool empty() const {
            return !(openCursorsOnDataShards.has_value() || openCursorOnConfigServer.has_value() ||
                     closeCursorsOnDataShards.has_value() || closeCursorOnConfigServer);
        }
    };

    boost::intrusive_ptr<ExpressionContext> _expCtx;
    OperationContext* _opCtx;
    exec::agg::ChangeStreamHandleTopologyChangeV2Stage::CursorManager& _cursorManager;
    const bool _degradedMode;

    CursorRequests _bufferedRequests;
};

/**
 * A simple waiter implementation that waits on the OperationContext until a deadline is reached
 * or the operation is interrupted.
 */
class OperationContextDeadlineWaiter final
    : public exec::agg::ChangeStreamHandleTopologyChangeV2Stage::DeadlineWaiter {
public:
    void waitUntil(OperationContext* opCtx, Date_t deadline) override {
        // This can throw, but "time limit exceeded" errors will be caught by the caller.
        opCtx->sleepUntil(deadline);
    }
};

}  // namespace

boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamHandleTopologyChangeV2ToStageFn(
    const boost::intrusive_ptr<const DocumentSource>& documentSource) {
    auto ds = boost::dynamic_pointer_cast<const DocumentSourceChangeStreamHandleTopologyChangeV2>(
        documentSource);

    tassert(10657523, "expected 'DocumentSourceChangeStreamHandleTopologyChangeV2' type", ds);

    const auto& expCtx = ds->getExpCtx();

    // Read data-to-shards allocation query service poll period from server parameter.
    auto pollPeriod = minAllocationToShardsPollPeriodSecs.loadRelaxed();

    ChangeStream changeStream = ChangeStream::buildFromExpressionContext(expCtx);

    auto* serviceContext = expCtx->getOperationContext()->getServiceContext();
    auto readerBuilder = ChangeStreamReaderBuilderImpl::get(serviceContext);
    auto dataToShardsAllocationQueryService =
        DataToShardsAllocationQueryService::get(serviceContext);

    auto params = std::make_shared<exec::agg::ChangeStreamHandleTopologyChangeV2Stage::Parameters>(
        changeStream,
        change_stream::resolveResumeTokenFromSpec(expCtx, *expCtx->getChangeStreamSpec()),
        pollPeriod,
        std::make_unique<OperationContextDeadlineWaiter>(),
        std::make_shared<V2StageCursorManager>(changeStream, readerBuilder),
        readerBuilder,
        dataToShardsAllocationQueryService);

    return make_intrusive<exec::agg::ChangeStreamHandleTopologyChangeV2Stage>(expCtx,
                                                                              std::move(params));
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(_internalChangeStreamHandleTopologyChangeV2,
                           DocumentSourceChangeStreamHandleTopologyChangeV2::id,
                           documentSourceChangeStreamHandleTopologyChangeV2ToStageFn)

ChangeStreamHandleTopologyChangeV2Stage::ChangeStreamHandleTopologyChangeV2Stage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::shared_ptr<ChangeStreamHandleTopologyChangeV2Stage::Parameters> params)
    : Stage(DocumentSourceChangeStreamHandleTopologyChangeV2::kStageName, expCtx),
      _params(std::move(params)) {}

StringData ChangeStreamHandleTopologyChangeV2Stage::stateToString(
    ChangeStreamHandleTopologyChangeV2Stage::State state) {
    switch (state) {
        case State::kUninitialized:
            return "Uninitialized";
        case State::kWaiting:
            return "Waiting";
        case State::kFetchingInitialization:
            return "FetchingInitialization";
        case State::kFetchingGettingChangeEvent:
            return "FetchingGettingChangeEvent";
        case State::kFetchingStartingChangeStreamSegment:
            return "FetchingStartingChangeStreamSegment";
        case State::kFetchingNormalGettingChangeEvent:
            return "FetchingNormalGettingChangeEvent";
        case State::kFetchingDegradedGettingChangeEvent:
            return "FetchingDegradedGettingChangeEvent";
        case State::kDowngrading:
            return "Downgrading";
        case State::kFinal:
            return "Final";
    }

    MONGO_UNREACHABLE_TASSERT(10657548);
}

exec::agg::MergeCursorsStage* ChangeStreamHandleTopologyChangeV2Stage::getSourceStage() const {
    auto source = dynamic_cast<exec::agg::MergeCursorsStage*>(pSource);
    tassert(10657509, "expecting source stage to be a MergeCursorsStage", source);
    return source;
}

boost::optional<DocumentSource::GetNextResult>
ChangeStreamHandleTopologyChangeV2Stage::runGetNextStateMachine_forTest() {
    return _runGetNextStateMachine();
}

void ChangeStreamHandleTopologyChangeV2Stage::setState_forTest(State state,
                                                               bool validateStateTransition) {
    if (validateStateTransition) {
        // Validate state transition. This will tassert if the state transition is not allowed.
        _setState(state);
    } else {
        // Only set the target state without validating the state transition.
        _state = state;
    }
}

Timestamp ChangeStreamHandleTopologyChangeV2Stage::extractTimestampFromDocument(
    const Document& input) {
    return ResumeToken::parse(input[DocumentSourceChangeStream::kIdField].getDocument())
        .getData()
        .clusterTime;
}

DocumentSource::GetNextResult ChangeStreamHandleTopologyChangeV2Stage::doGetNext() {
    // Continue advancing the state until there is either an event to return or an exception is
    // thrown.
    // TODO SERVER-110795: add some iteration-based or time-abort abort criteria to this loop so it
    // does not iterate forever in case of a programming error.
    for (;;) {
        auto event = _runGetNextStateMachine();
        if (event.has_value()) {
            return *event;
        }

        // The following allows breaking out of the state machine eventually if the query has been
        // interrupted.
        pExpCtx->checkForInterrupt();
    }
}

boost::optional<DocumentSource::GetNextResult>
ChangeStreamHandleTopologyChangeV2Stage::_runGetNextStateMachine() {
    LOGV2_DEBUG(10657500,
                3,
                STAGE_LOG_PREFIX "Executing state machine",
                "state"_attr = stateToString(_state),
                "changeStream"_attr = _params->changeStream,
                "resumeTokenClusterTime"_attr = _params->resumeToken.clusterTime);

    try {
        switch (_state) {
            case State::kUninitialized:
                return _handleStateUninitialized();
            case State::kWaiting:
                return _handleStateWaiting();
            case State::kFetchingInitialization:
                return _handleStateFetchingInitialization();
            case State::kFetchingGettingChangeEvent:
                return _handleStateFetchingGettingChangeEvent();
            case State::kFetchingStartingChangeStreamSegment:
                return _handleStateFetchingStartingChangeStreamSegment();
            case State::kFetchingNormalGettingChangeEvent:
                return _handleStateFetchingNormalGettingChangeEvent();
            case State::kFetchingDegradedGettingChangeEvent:
                return _handleStateFetchingDegradedGettingChangeEvent();
            case State::kDowngrading:
                // Called method does not return a value and always throws.
                _handleStateDowngrading();
            case State::kFinal:
                // Rethrow last error.
                tassert(10657532, "expecting _lastError to be set", !_lastError.isOK());
                error_details::throwExceptionForStatus(_lastError);
        }

        MONGO_UNREACHABLE_TASSERT(10657502);
    } catch (const ExceptionFor<ErrorCodes::ShardNotFound>& ex) {
        // Catch any 'ShardNotFound' exceptions and convert them into more appropriate errors.
        // Any errors other than 'ShardNotFound' will escape from this method intentionally.
        if (_params->changeStream.getReadMode() == ChangeStreamReadMode::kStrict) {
            // In strict mode, rethrow any 'ShardNotFound' error as 'ShardRemovedError'.
            _lastError =
                Status(ErrorCodes::ShardRemovedError,
                       fmt::format("unable to establish all necessary cursors as at least one "
                                   "required shard has been removed. {}",
                                   ex.reason()));
        } else {
            // In ignoreRemovedShards mode, rethrow any 'ShardNotFound' error as
            // 'RetryChangeStream' error.
            _lastError =
                Status(ErrorCodes::RetryChangeStream,
                       fmt::format("unable to establish all necessary cursors as at least one "
                                   "shard has been removed. {}",
                                   ex.reason()));
        }
        _setState(State::kFinal);
        error_details::throwExceptionForStatus(_lastError);
    } catch (const DBException& ex) {
        Status status = ex.toStatus();

        LOGV2_WARNING(10657501,
                      STAGE_LOG_PREFIX "Caught exception in state machine",
                      "state"_attr = stateToString(_state),
                      "error"_attr = redact(status),
                      "changeStream"_attr = _params->changeStream,
                      "resumeTokenClusterTime"_attr = _params->resumeToken.clusterTime);

        // We cannot set state to kFinal if we have already been in state kFinal before.
        if (_state != State::kFinal) {
            _lastError = status;
            _setState(State::kFinal);
        }
        throw;
    }
}

void ChangeStreamHandleTopologyChangeV2Stage::_setState(
    ChangeStreamHandleTopologyChangeV2Stage::State newState) {
    // Cannot set the state to the current state again.
    tassert(10657503,
            str::stream() << "invalid repeated state assignment for state "
                          << stateToString(newState),
            _state != newState);

    // Cannot transition from state kFinal to another state.
    tassert(10657504,
            str::stream() << "cannot transition state from " << stateToString(State::kFinal)
                          << " to " << stateToString(newState),
            _state != State::kFinal);

    // Cannot transition back to stage kUninitialized.
    tassert(10657505,
            str::stream() << "cannot transition state back to " << stateToString(newState),
            newState != State::kUninitialized);

    // Cannot transition to stage kWaiting except from kUninitialized.
    tassert(10657529,
            str::stream() << "cannot transition state to " << stateToString(newState),
            newState != State::kWaiting || _state == State::kUninitialized);

    // Cannot transition to stage kFetchingInitialization except from kUninitialized or kWaiting.
    tassert(10657530,
            str::stream() << "cannot transition state to " << stateToString(newState),
            newState != State::kFetchingInitialization || _state == State::kUninitialized ||
                _state == State::kWaiting);

    LOGV2_DEBUG(10657506,
                3,
                STAGE_LOG_PREFIX "Transitioning state",
                "previous"_attr = stateToString(_state),
                "new"_attr = stateToString(newState),
                "changeStream"_attr = _params->changeStream,
                "resumeTokenClusterTime"_attr = _params->resumeToken.clusterTime);

    _state = newState;
}

void ChangeStreamHandleTopologyChangeV2Stage::_assertState(
    ChangeStreamHandleTopologyChangeV2Stage::State expectedState,
    boost::optional<ChangeStreamReadMode> expectedMode,
    StringData context) const {
    tassert(10657508,
            str::stream() << "unexpected state in " << context << "(), expecting: "
                          << stateToString(expectedState) << ", actual: " << stateToString(_state),
            _state == expectedState);

    auto modeToString = [](ChangeStreamReadMode mode) -> StringData {
        if (mode == ChangeStreamReadMode::kStrict) {
            return "strict";
        }
        return "ignoreRemovedShards";
    };

    if (expectedMode.has_value()) {
        auto actualMode = _params->changeStream.getReadMode();
        tassert(10657515,
                str::stream() << "expecting change stream to be opened in mode "
                              << modeToString(*expectedMode) << ", but got "
                              << modeToString(actualMode) << " in " << context << "()",
                *expectedMode == actualMode);
    }
}

void ChangeStreamHandleTopologyChangeV2Stage::_ensureShardTargeter() {
    if (_shardTargeter == nullptr) {
        _shardTargeter = _params->changeStreamReaderBuilder->buildShardTargeter(
            getContext()->getOperationContext(), _params->changeStream);
    }
}

void ChangeStreamHandleTopologyChangeV2Stage::_logShardTargeterDecision(
    StringData context, ShardTargeterDecision targeterDecision) const {
    LOGV2_DEBUG(10657549,
                3,
                STAGE_LOG_PREFIX "Shard targeter decision",
                "state"_attr = stateToString(_state),
                "context"_attr = context,
                "decision"_attr = targeterDecision,
                "changeStream"_attr = _params->changeStream,
                "resumeTokenClusterTime"_attr = _params->resumeToken.clusterTime);
}

void ChangeStreamHandleTopologyChangeV2Stage::_logShardTargeterDecision(
    StringData context, ShardTargeterDecision targeterDecision, const Document& event) const {
    LOGV2_DEBUG(10657557,
                3,
                STAGE_LOG_PREFIX "Shard targeter decision",
                "state"_attr = stateToString(_state),
                "context"_attr = context,
                "decision"_attr = targeterDecision,
                "event"_attr = event,
                "changeStream"_attr = _params->changeStream,
                "resumeTokenClusterTime"_attr = _params->resumeToken.clusterTime);
}

void ChangeStreamHandleTopologyChangeV2Stage::_logShardTargeterDecision(
    StringData context,
    ShardTargeterDecision targeterDecision,
    Timestamp segmentStart,
    Timestamp segmentEnd) const {
    LOGV2_DEBUG(10657558,
                3,
                STAGE_LOG_PREFIX "Shard targeter decision",
                "state"_attr = stateToString(_state),
                "context"_attr = context,
                "decision"_attr = targeterDecision,
                "segmentStart"_attr = segmentStart,
                "segmentEnd"_attr = segmentEnd,
                "changeStream"_attr = _params->changeStream,
                "resumeTokenClusterTime"_attr = _params->resumeToken.clusterTime);
}

AllocationToShardsStatus ChangeStreamHandleTopologyChangeV2Stage::_getAllocationToShardsStatus(
    Timestamp clusterTime) {
    // Save old previous request time for logging purposes.
    Date_t previousRequestTime = _lastAllocationToShardsRequestTime;
    _lastAllocationToShardsRequestTime =
        getContext()->getOperationContext()->getServiceContext()->getPreciseClockSource()->now();

    AllocationToShardsStatus allocationToShardsStatus =
        _params->dataToShardsAllocationQueryService->getAllocationToShardsStatus(
            getContext()->getOperationContext(), clusterTime);

    LOGV2_DEBUG(10657550,
                3,
                STAGE_LOG_PREFIX "Allocation to shards status",
                "state"_attr = stateToString(_state),
                "previousRequestTime"_attr = previousRequestTime,
                "clusterTime"_attr = clusterTime,
                "status"_attr = allocationToShardsStatus,
                "changeStream"_attr = _params->changeStream,
                "resumeTokenClusterTime"_attr = _params->resumeToken.clusterTime);

    return allocationToShardsStatus;
}

boost::optional<DocumentSource::GetNextResult>
ChangeStreamHandleTopologyChangeV2Stage::_handleStateUninitialized() {
    _assertState(State::kUninitialized, {} /* mode */, "_handleStateUninitialized");

    auto allocationToShardsStatus = _getAllocationToShardsStatus(_params->resumeToken.clusterTime);

    if (allocationToShardsStatus == AllocationToShardsStatus::kNotAvailable) {
        uasserted(ErrorCodes::RetryChangeStream,
                  "No information about collection/database allocation to data shards is "
                  "available for the requested cluster time");
    }

    _params->cursorManager->initialize(getContext(), this, _params->resumeToken);

    switch (allocationToShardsStatus) {
        case AllocationToShardsStatus::kFutureClusterTime:
            _setState(State::kWaiting);
            return boost::none;
        case AllocationToShardsStatus::kOk:
            _setState(State::kFetchingInitialization);
            return boost::none;
        case AllocationToShardsStatus::kNotAvailable:
            break;
    }

    MONGO_UNREACHABLE_TASSERT(10657511);
}

boost::optional<DocumentSource::GetNextResult>
ChangeStreamHandleTopologyChangeV2Stage::_handleStateWaiting() {
    _assertState(State::kWaiting, {} /* mode */, "_handleStateWaiting");

    OperationContext* opCtx = getContext()->getOperationContext();
    Date_t now = opCtx->getServiceContext()->getPreciseClockSource()->now();
    Seconds secondsSinceLastPoll = duration_cast<Seconds>(now - _lastAllocationToShardsRequestTime);

    if (secondsSinceLastPoll < Seconds(_params->minAllocationToShardsPollPeriodSecs)) {
        // Wait until the next poll time.
        Date_t nextPollTime = _lastAllocationToShardsRequestTime +
            Seconds(_params->minAllocationToShardsPollPeriodSecs);
        try {
            // The following call throws if the operation got interrupted or killed, or if the
            // OperationContext's own deadline (maxAwaitTimeMS) has been exceeded. Does not throw if
            // waiting reached 'nextPollTime', but the OperationContext's deadline has not yet
            // expired.
            _params->deadlineWaiter->waitUntil(opCtx, nextPollTime);
        } catch (const ExceptionFor<ErrorCategory::ExceededTimeLimitError>& ex) {
            // OperationContext deadline exceeded. Return EOF so the client gets an intermediate
            // result back.
            LOGV2_DEBUG(10657544,
                        3,
                        STAGE_LOG_PREFIX "Deadline time limit exceeded",
                        "nextPollTime"_attr = nextPollTime,
                        "deadline"_attr = opCtx->getDeadline(),
                        "state"_attr = stateToString(_state),
                        "error"_attr = redact(ex.toStatus()),
                        "changeStream"_attr = _params->changeStream,
                        "resumeTokenClusterTime"_attr = _params->resumeToken.clusterTime);

            return GetNextResult::makeEOF();
        }

        // No state change here, so we enter the state machine in the next turn with kWaiting
        // again.
        return boost::none;
    }

    // Poll data-to-shards allocation again.
    switch (_getAllocationToShardsStatus(_params->resumeToken.clusterTime)) {
        case AllocationToShardsStatus::kNotAvailable:
            // No placement information is available for the specified cluster time. Throw
            // 'RetryChangeStream' exception.
            uasserted(ErrorCodes::RetryChangeStream,
                      "Could not retrieve placement information for the specified cluster time");
        case AllocationToShardsStatus::kFutureClusterTime:
            // Cluster time is still in the future. Return EOF to the client because
            // maxAwaitTimeMS has expired.
            return GetNextResult::makeEOF();
        case AllocationToShardsStatus::kOk:
            // Transition to kFetchingInitialization state.
            _setState(State::kFetchingInitialization);
            return boost::none;
    }

    MONGO_UNREACHABLE_TASSERT(10657507);
}

boost::optional<DocumentSource::GetNextResult>
ChangeStreamHandleTopologyChangeV2Stage::_handleStateFetchingInitialization() {
    _assertState(
        State::kFetchingInitialization, {} /* mode */, "_handleStateFetchingInitialization");

    tassert(10657513, "should not have shardTargeter object", _shardTargeter == nullptr);

    _ensureShardTargeter();

    if (_params->changeStream.getReadMode() == ChangeStreamReadMode::kIgnoreRemovedShards) {
        // Enter state machine for ignoreRemovedShards mode.
        _segmentStartTimestamp = _params->resumeToken.clusterTime;

        // Reset failure counter.
        _shardNotFoundFailuresInARow = 0;

        _setState(State::kFetchingStartingChangeStreamSegment);
        return boost::none;
    }

    // Strict mode.

    V2StageReaderContext readerContext(getContext(),
                                       getContext()->getOperationContext(),
                                       *_params->cursorManager,
                                       false /* degradedMode */);

    auto shardTargeterDecision = _shardTargeter->initialize(
        getContext()->getOperationContext(), _params->resumeToken.clusterTime, readerContext);

    _logShardTargeterDecision("Initialized shardTargeter", shardTargeterDecision);

    switch (shardTargeterDecision) {
        case ShardTargeterDecision::kContinue:
            // Opens and closes the cursors as requested in the reader context. If this throws, the
            // exception bubbles up and we transition to the kFinal state.
            readerContext.executeCursorRequests();
            _setState(State::kFetchingGettingChangeEvent);
            return boost::none;
        case ShardTargeterDecision::kSwitchToV1:
            _setState(State::kDowngrading);
            return DocumentSource::GetNextResult::makeEOF();
    }

    MONGO_UNREACHABLE_TASSERT(10657512);
}

boost::optional<DocumentSource::GetNextResult>
ChangeStreamHandleTopologyChangeV2Stage::_handleStateFetchingGettingChangeEvent() {
    _assertState(State::kFetchingGettingChangeEvent,
                 ChangeStreamReadMode::kStrict /* mode */,
                 "_handleStateFetchingGettingChangeEvent");

    auto input = pSource->getNext();
    if (!input.isAdvancedControlDocument()) {
        // Intentionally no state change here.
        return input;
    }

    // Advanced control document found.

    // Shard targeter should always be already present here in non-testing mode, but it may be
    // missing when unit testing the individual states.
    _ensureShardTargeter();

    V2StageReaderContext readerContext(getContext(),
                                       getContext()->getOperationContext(),
                                       *_params->cursorManager,
                                       false /* degradedMode */);

    auto shardTargeterDecision = _shardTargeter->handleEvent(
        getContext()->getOperationContext(), input.getDocument(), readerContext);

    _logShardTargeterDecision("handleEvent", shardTargeterDecision, input.getDocument());

    switch (shardTargeterDecision) {
        case ShardTargeterDecision::kContinue:
            // Opens and closes the cursors as requested in the reader context. If this throws, the
            // exception bubbles up and we go into the kFinal state.
            readerContext.executeCursorRequests();
            return boost::none;
        case ShardTargeterDecision::kSwitchToV1:
            _setState(State::kDowngrading);
            return DocumentSource::GetNextResult::makeEOF();
    }

    MONGO_UNREACHABLE_TASSERT(10657514);
}

boost::optional<DocumentSource::GetNextResult>
ChangeStreamHandleTopologyChangeV2Stage::_handleStateFetchingStartingChangeStreamSegment() {
    _assertState(State::kFetchingStartingChangeStreamSegment,
                 ChangeStreamReadMode::kIgnoreRemovedShards /* mode */,
                 "_handleStateFetchingStartingChangeStreamSegment");

    // Shard targeter should always be already present here in non-testing mode, but it may be
    // missing when unit testing the individual states.
    _ensureShardTargeter();

    tassert(10657518,
            "expecting segment start timestamp to be set",
            _segmentStartTimestamp.has_value());

    ON_BLOCK_EXIT([&]() {
        // Turn on undo buffering in the results merger if we are entering the degraded fetching
        // state.
        if (_state == State::kFetchingDegradedGettingChangeEvent) {
            _params->cursorManager->enableUndoNextMode();
        }
    });

    V2StageReaderContext readerContext(getContext(),
                                       getContext()->getOperationContext(),
                                       *_params->cursorManager,
                                       true /* degradedMode */);

    auto [shardTargeterDecision, segmentEndTimestamp] = _shardTargeter->startChangeStreamSegment(
        getContext()->getOperationContext(), *_segmentStartTimestamp, readerContext);

    _logShardTargeterDecision("Started change stream segment",
                              shardTargeterDecision,
                              *_segmentStartTimestamp,
                              segmentEndTimestamp.value_or(Timestamp::max()));

    switch (shardTargeterDecision) {
        case ShardTargeterDecision::kSwitchToV1:
            _setState(State::kDowngrading);
            return GetNextResult::makeEOF();
        case ShardTargeterDecision::kContinue:
            // Opens and closes the cursors as requested in the reader context. If we catch a
            // 'ShardNotFound' error, we continue. All other exceptions bubble up and cause a
            // transition to the kFinal state.
            try {
                readerContext.executeCursorRequests();
            } catch (const ExceptionFor<ErrorCodes::ShardNotFound>& ex) {
                LOGV2_DEBUG(10657542,
                            3,
                            STAGE_LOG_PREFIX "Encountered 'ShardNotFound' error",
                            "state"_attr = stateToString(_state),
                            "error"_attr = redact(ex.toStatus()),
                            "shardNotFoundFailuresInARow"_attr = _shardNotFoundFailuresInARow,
                            "maxShardNotFoundFailuresInARow"_attr = kMaxShardNotFoundFailuresInARow,
                            "changeStream"_attr = _params->changeStream,
                            "resumeTokenClusterTime"_attr = _params->resumeToken.clusterTime);

                // Avoid an infinite loop here by counting the number of failures in a row, and
                // break out with a tassert if we have already seen too many.
                tassert(10657541,
                        "encountered too many consecutive 'ShardNotFound' errors",
                        ++_shardNotFoundFailuresInARow < kMaxShardNotFoundFailuresInARow);

                // No state change has happened yet. We are returning nothing here so the state
                // machine will run again for the same state.
                return boost::none;
            }

            // State change to either degraded or normal fetching.
            _segmentEndTimestamp = segmentEndTimestamp;
            if (segmentEndTimestamp.has_value()) {
                _setState(State::kFetchingDegradedGettingChangeEvent);
            } else {
                _setState(State::kFetchingNormalGettingChangeEvent);
            }
            return boost::none;
    }

    MONGO_UNREACHABLE_TASSERT(10657519);
}

boost::optional<DocumentSource::GetNextResult>
ChangeStreamHandleTopologyChangeV2Stage::_handleStateFetchingNormalGettingChangeEvent() {
    _assertState(State::kFetchingNormalGettingChangeEvent,
                 ChangeStreamReadMode::kIgnoreRemovedShards /* mode */,
                 "_handleStateFetchingNormalGettingChangeEvent");

    auto input = pSource->getNext();
    if (!input.isAdvancedControlDocument()) {
        // Intentionally no state change here.
        return input;
    }

    // Advanced control document found.

    // Shard targeter should always be already present here in non-testing mode, but it may be
    // missing when unit testing the individual states.
    _ensureShardTargeter();

    ON_BLOCK_EXIT([&]() {
        // Turn on undo buffering in the results merger if we are entering the degraded fetching
        // state.
        if (_state == State::kFetchingDegradedGettingChangeEvent) {
            _params->cursorManager->enableUndoNextMode();
        }
    });

    V2StageReaderContext readerContext(getContext(),
                                       getContext()->getOperationContext(),
                                       *_params->cursorManager,
                                       false /* degradedMode */);

    auto shardTargeterDecision = _shardTargeter->handleEvent(
        getContext()->getOperationContext(), input.getDocument(), readerContext);

    _logShardTargeterDecision(
        "Handle event (fetching normal)", shardTargeterDecision, input.getDocument());

    switch (shardTargeterDecision) {
        case ShardTargeterDecision::kContinue:
            // Opens and closes the cursors as requested in the reader context. If we catch a
            // 'ShardNotFound' exception, we go into degraded mode.
            try {
                readerContext.executeCursorRequests();
            } catch (const ExceptionFor<ErrorCodes::ShardNotFound>& ex) {
                // Adjust end timestamp of the current segment and transition to degraded mode.
                _segmentEndTimestamp =
                    ResumeToken::parse(
                        input.getDocument()[DocumentSourceChangeStream::kIdField].getDocument())
                        .getData()
                        .clusterTime +
                    1;

                LOGV2_DEBUG(10657543,
                            3,
                            STAGE_LOG_PREFIX
                            "Encountered 'ShardNotFound' error. Adjusting end timestamp for "
                            "segment and transitioning to degraded mode",
                            "state"_attr = stateToString(_state),
                            "error"_attr = redact(ex.toStatus()),
                            "endTimestamp"_attr = _segmentEndTimestamp.value(),
                            "changeStream"_attr = _params->changeStream,
                            "resumeTokenClusterTime"_attr = _params->resumeToken.clusterTime);

                _setState(State::kFetchingDegradedGettingChangeEvent);
                // Note: the current control event is lost here and will not be processed again.
            }
            return boost::none;
        case ShardTargeterDecision::kSwitchToV1:
            _setState(State::kDowngrading);
            return DocumentSource::GetNextResult::makeEOF();
    }

    MONGO_UNREACHABLE_TASSERT(10657520);
}

boost::optional<DocumentSource::GetNextResult>
ChangeStreamHandleTopologyChangeV2Stage::_handleStateFetchingDegradedGettingChangeEvent() {
    _assertState(State::kFetchingDegradedGettingChangeEvent,
                 ChangeStreamReadMode::kIgnoreRemovedShards /* mode */,
                 "_handleStateFetchingDegradedGettingChangeEvent");

    tassert(
        10657521, "expecting segment end timestamp to be set", _segmentEndTimestamp.has_value());

    ON_BLOCK_EXIT([&]() {
        // Turn off undo buffering in the results merger if we are exiting this state.
        if (_state != State::kFetchingDegradedGettingChangeEvent) {
            _params->cursorManager->disableUndoNextMode();
        }
    });

    auto input = pSource->getNext();

    Timestamp eventTimestamp = [&]() {
        if (input.isAdvancedControlDocument()) {
            // Extract the cluster time from the current event, via the "_id" field. This requires
            // the "_id" field to be present for all events on mongos. It also requires parsing the
            // resume token data, so we limit it to control events.
            return extractTimestampFromDocument(input.getDocument());
        }

        // Extract the cluster time from the current high-water mark of the 'AsyncResultsMerger'.
        return _params->cursorManager->getTimestampFromCurrentHighWaterMark();
    }();

    if (eventTimestamp >= *_segmentEndTimestamp) {
        _segmentStartTimestamp = _segmentEndTimestamp;
        _segmentEndTimestamp.reset();

        // Undo the effects of fetching the last event in the underlying results merger.
        _params->cursorManager->undoGetNextAndSetHighWaterMark(*_segmentStartTimestamp);

        // Reset failure counter.
        _shardNotFoundFailuresInARow = 0;
        _setState(State::kFetchingStartingChangeStreamSegment);
    }

    if (!input.isAdvancedControlDocument()) {
        return input;
    }

    // Advanced control document found.

    // Shard targeter should always be already present here in non-testing mode, but it may be
    // missing when unit testing the individual states.
    _ensureShardTargeter();

    V2StageReaderContext readerContext(getContext(),
                                       getContext()->getOperationContext(),
                                       *_params->cursorManager,
                                       true /* degradedMode */);

    auto shardTargeterDecision = _shardTargeter->handleEvent(
        getContext()->getOperationContext(), input.getDocument(), readerContext);

    _logShardTargeterDecision("Handle event (fetching degraded)", shardTargeterDecision);

    switch (shardTargeterDecision) {
        case ShardTargeterDecision::kContinue:
            // Opening and closing cursors should not happen in degraded mode, so we intentionally
            // do not call 'executeCursorRequests()' on the 'readerContext' here.
            tassert(10657556,
                    "should not have buffered any open/close requests in degraded fetching mode",
                    !readerContext.hasBufferedCursorRequests());
            return boost::none;
        case ShardTargeterDecision::kSwitchToV1:
            _setState(State::kDowngrading);
            return DocumentSource::GetNextResult::makeEOF();
    }

    MONGO_UNREACHABLE_TASSERT(10657522);
}

void ChangeStreamHandleTopologyChangeV2Stage::_handleStateDowngrading() {
    _assertState(State::kDowngrading, {} /* mode */, "_handleStateDowngrading");

    uasserted(ErrorCodes::RetryChangeStream, "Downgrading change stream reader to v1 version");
}

}  // namespace exec::agg
}  // namespace mongo
