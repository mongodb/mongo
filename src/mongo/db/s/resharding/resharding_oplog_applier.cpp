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


#include "mongo/db/s/resharding/resharding_oplog_applier.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_donor_oplog_iterator.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"
#include "mongo/util/timer.h"
#include "mongo/util/uuid.h"

#include <mutex>
#include <string>
#include <tuple>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding


namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(reshardingApplyOplogBatchTwice);

/*
 * Returns the amount of time that has elapsed since the oplog entry was created.
 */
Milliseconds calculateTimeElapsedSinceOplogWallClockTime(OperationContext* opCtx,
                                                         const repl::OplogEntry& oplogEntry) {
    auto oplogWallTime = oplogEntry.getWallClockTime();
    auto currentWallTime = opCtx->fastClockSource().now();
    // If there are clock skews, then the difference below may be negative so cap it at zero.
    return std::max(Milliseconds(0), currentWallTime - oplogWallTime);
}

}  // namespace

ReshardingOplogApplier::ReshardingOplogApplier(
    std::unique_ptr<Env> env,
    std::size_t oplogBatchTaskCount,
    ReshardingSourceId sourceId,
    NamespaceString oplogBufferNss,
    NamespaceString outputNss,
    std::vector<NamespaceString> allStashNss,
    size_t myStashIdx,
    ChunkManager sourceChunkMgr,
    std::unique_ptr<ReshardingDonorOplogIteratorInterface> oplogIterator,
    bool isCapped)
    : _env(std::move(env)),
      _sourceId(std::move(sourceId)),
      _batchPreparer{oplogBatchTaskCount,
                     CollatorInterface::cloneCollator(sourceChunkMgr.getDefaultCollator()),
                     isCapped},
      _crudApplication{std::move(outputNss),
                       std::move(allStashNss),
                       myStashIdx,
                       _sourceId.getShardId(),
                       std::move(sourceChunkMgr),
                       _env->applierMetrics(),
                       isCapped},
      _sessionApplication{std::move(oplogBufferNss)},
      _batchApplier{_crudApplication, _sessionApplication},
      _oplogIter(std::move(oplogIterator)) {}

SemiFuture<void> ReshardingOplogApplier::_applyBatch(
    std::shared_ptr<executor::TaskExecutor> executor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory factory) {
    Timer latencyTimer;
    auto crudWriterVectors = _batchPreparer.makeCrudOpWriterVectors(
        _currentBatchToApply, _currentDerivedOpsForCrudWriters);

    CancellationSource errorSource(cancelToken);

    std::vector<SharedSemiFuture<void>> batchApplierFutures;
    // Use `2 * crudWriterVectors.size()` because sessionWriterVectors.size() is very likely equal
    // to crudWriterVectors.size(). Calling ReshardingOplogBatchApplier::applyBatch<false>() first
    // though allows CRUD application to be concurrent with preparing the writer vectors for session
    // application in addition to being concurrent with session application itself.
    batchApplierFutures.reserve(2 * crudWriterVectors.size());

    for (auto&& writer : crudWriterVectors) {
        if (!writer.empty()) {
            batchApplierFutures.emplace_back(
                _batchApplier
                    .applyBatch<false>(std::move(writer), executor, errorSource.token(), factory)
                    .share());
        }
    }

    auto sessionWriterVectors = _batchPreparer.makeSessionOpWriterVectors(
        _currentBatchToApply, _currentDerivedOpsForSessionWriters);
    batchApplierFutures.reserve(crudWriterVectors.size() + sessionWriterVectors.size());

    for (auto&& writer : sessionWriterVectors) {
        if (!writer.empty()) {
            batchApplierFutures.emplace_back(
                _batchApplier
                    .applyBatch<true>(std::move(writer), executor, errorSource.token(), factory)
                    .share());
        }
    }

    return resharding::cancelWhenAnyErrorThenQuiesce(batchApplierFutures, executor, errorSource)
        .onError([](Status status) {
            LOGV2_ERROR(
                5012004, "Failed to apply operation in resharding", "error"_attr = redact(status));
            return status;
        })
        .onCompletion([this, latencyTimer](Status status) {
            _env->applierMetrics()->onOplogLocalBatchApplied(
                duration_cast<Milliseconds>(latencyTimer.elapsed()));

            return status;
        })
        .semi();
}

SemiFuture<void> ReshardingOplogApplier::run(
    std::shared_ptr<executor::TaskExecutor> executor,
    std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory factory) {
    struct ChainContext {
        std::unique_ptr<ReshardingDonorOplogIteratorInterface> oplogIter;
        Timer fetchTimer;
    };

    auto chainCtx = std::make_shared<ChainContext>();
    chainCtx->oplogIter = std::move(_oplogIter);

    return AsyncTry([this, chainCtx, executor, cancelToken, factory] {
               chainCtx->fetchTimer.reset();
               return chainCtx->oplogIter->getNextBatch(executor, cancelToken, factory)
                   .thenRunOn(executor)
                   .then([this, chainCtx, executor, cancelToken, factory](OplogBatch batch) {
                       LOGV2_DEBUG(5391002, 3, "Starting batch", "batchSize"_attr = batch.size());

                       _env->applierMetrics()->onBatchRetrievedDuringOplogApplying(
                           duration_cast<Milliseconds>(chainCtx->fetchTimer.elapsed()));

                       _currentBatchToApply = std::move(batch);
                       return _applyBatch(executor, cancelToken, factory);
                   })
                   .then([this, executor, cancelToken, factory] {
                       if (MONGO_unlikely(reshardingApplyOplogBatchTwice.shouldFail())) {
                           LOGV2(5687600,
                                 "reshardingApplyOplogBatchTwice failpoint enabled, applying batch "
                                 "a second time",
                                 "batchSize"_attr = _currentBatchToApply.size());
                           _currentDerivedOpsForCrudWriters.clear();
                           _currentDerivedOpsForSessionWriters.clear();
                           return _applyBatch(executor, cancelToken, factory);
                       }
                       return SemiFuture<void>();
                   })
                   .then([this, factory] {
                       if (_currentBatchToApply.empty()) {
                           // Increment the number of entries applied by 1 in order to account for
                           // the final oplog entry that the iterator never returns because it's a
                           // known no-op oplog entry.
                           _env->applierMetrics()->onOplogEntriesApplied(1);

                           return false;
                       }

                       auto opCtx = factory.makeOperationContext(&cc());
                       _clearAppliedOpsAndStoreProgress(opCtx.get());
                       return true;
                   });
           })
        .until([](const StatusWith<bool>& swMoreToApply) {
            return !swMoreToApply.isOK() || !swMoreToApply.getValue();
        })
        .on(executor, cancelToken)
        .ignoreValue()
        .thenRunOn(std::move(cleanupExecutor))
        // It is unsafe to capture `this` once the task is running on the cleanupExecutor because
        // RecipientStateMachine, along with its ReshardingOplogApplier member, may have already
        // been destructed.
        .onCompletion([chainCtx](Status status) {
            if (chainCtx->oplogIter) {
                // Use a separate Client to make a better effort of calling dispose() even when the
                // CancellationToken has been canceled.
                //
                // TODO(SERVER-111752): Please revisit if this thread could be made killable.
                auto client = cc().getServiceContext()
                                  ->getService(ClusterRole::ShardServer)
                                  ->makeClient("ReshardingOplogApplierCleanupClient",
                                               Client::noSession(),
                                               ClientOperationKillableByStepdown{false});

                AlternativeClientRegion acr(client);
                auto opCtx = cc().makeOperationContext();

                chainCtx->oplogIter->dispose(opCtx.get());
                chainCtx->oplogIter.reset();
            }

            return status;
        })
        .semi();
}

boost::optional<ReshardingOplogApplierProgress> ReshardingOplogApplier::checkStoredProgress(
    OperationContext* opCtx, const ReshardingSourceId& id) {
    DBDirectClient client(opCtx);
    auto doc = client.findOne(
        NamespaceString::kReshardingApplierProgressNamespace,
        BSON(ReshardingOplogApplierProgress::kOplogSourceIdFieldName << id.toBSON()));

    if (doc.isEmpty()) {
        return boost::none;
    }

    IDLParserContext ctx("ReshardingOplogApplierProgress");
    return ReshardingOplogApplierProgress::parse(doc, ctx);
}

void ReshardingOplogApplier::_clearAppliedOpsAndStoreProgress(OperationContext* opCtx) {
    const auto& lastOplog = _currentBatchToApply.back();

    auto oplogId = ReshardingDonorOplogId::parse(
        lastOplog.get_id()->getDocument().toBson(),
        IDLParserContext{"ReshardingOplogApplier::_clearAppliedOpsAndStoreProgress"});

    PersistentTaskStore<ReshardingOplogApplierProgress> store(
        NamespaceString::kReshardingApplierProgressNamespace);

    BSONObjBuilder builder;
    builder.append("$set",
                   BSON(ReshardingOplogApplierProgress::kProgressFieldName << oplogId.toBSON()));
    builder.append("$inc",
                   BSON(ReshardingOplogApplierProgress::kNumEntriesAppliedFieldName
                        << static_cast<long long>(_currentBatchToApply.size())));

    builder.append("$set",
                   BSON(ReshardingOplogApplierProgress::kInsertsAppliedFieldName
                        << _env->applierMetrics()->getInsertsApplied()));
    builder.append("$set",
                   BSON(ReshardingOplogApplierProgress::kUpdatesAppliedFieldName
                        << _env->applierMetrics()->getUpdatesApplied()));
    builder.append("$set",
                   BSON(ReshardingOplogApplierProgress::kDeletesAppliedFieldName
                        << _env->applierMetrics()->getDeletesApplied()));
    builder.append("$set",
                   BSON(ReshardingOplogApplierProgress::kWritesToStashCollectionsFieldName
                        << _env->applierMetrics()->getWritesToStashCollections()));

    store.upsert(
        opCtx,
        BSON(ReshardingOplogApplierProgress::kOplogSourceIdFieldName << _sourceId.toBSON()),
        builder.obj(),
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

    _env->applierMetrics()->onOplogEntriesApplied(_currentBatchToApply.size());

    if (_needToEstimateRemainingTimeBasedOnMovingAverage(opCtx)) {
        _updateAverageTimeToApplyOplogEntries(opCtx);
    }

    _currentBatchToApply.clear();
    _currentDerivedOpsForCrudWriters.clear();
    _currentDerivedOpsForSessionWriters.clear();
}

bool ReshardingOplogApplier::_needToEstimateRemainingTimeBasedOnMovingAverage(
    OperationContext* opCtx) {
    if (!_supportEstimatingRemainingTimeBasedOnMovingAverage.has_value()) {
        // Only check the feature flag once since the setFCV command aborts any in-progress
        // resharding operation so no resharding operations can span multiple FCV versions.
        _supportEstimatingRemainingTimeBasedOnMovingAverage =
            resharding::gFeatureFlagReshardingRemainingTimeEstimateBasedOnMovingAverage.isEnabled(
                VersionContext::getDecoration(opCtx),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
    }

    return *_supportEstimatingRemainingTimeBasedOnMovingAverage &&
        resharding::gReshardingRemainingTimeEstimateBasedOnMovingAverage.load();
}

void ReshardingOplogApplier::_updateAverageTimeToApplyOplogEntries(OperationContext* opCtx) {
    if (_currentBatchToApply.empty()) {
        return;
    }

    // Update the average based on the last oplog in the batch.
    auto timeToApply =
        calculateTimeElapsedSinceOplogWallClockTime(opCtx, _currentBatchToApply.back());
    _env->applierMetrics()->updateAverageTimeToApplyOplogEntries(timeToApply);
}

NamespaceString ReshardingOplogApplier::ensureStashCollectionExists(
    OperationContext* opCtx,
    const UUID& existingUUID,
    const ShardId& donorShardId,
    const CollectionOptions& options) {
    auto nss = resharding::getLocalConflictStashNamespace(existingUUID, donorShardId);

    resharding::data_copy::ensureCollectionExists(opCtx, nss, options);
    return nss;
}

}  // namespace mongo
