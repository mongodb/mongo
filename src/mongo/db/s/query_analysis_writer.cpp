/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include "mongo/platform/basic.h"

#include "mongo/db/s/query_analysis_writer.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/analyze_shard_key_server_parameters_gen.h"
#include "mongo/s/analyze_shard_key_util.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/concurrency/thread_pool.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace analyze_shard_key {

namespace {

MONGO_FAIL_POINT_DEFINE(disableQueryAnalysisWriter);

const auto getQueryAnalysisWriter = ServiceContext::declareDecoration<QueryAnalysisWriter>();


struct SampledWriteCommandRequest {
    UUID sampleId;
    NamespaceString nss;
    BSONObj cmd;  // the BSON for a {Update,Delete,FindAndModify}CommandRequest
};

/*
 * Returns a sampled update command for the update at 'opIndex' in the given update command.
 */
SampledWriteCommandRequest makeSampledUpdateCommandRequest(
    const write_ops::UpdateCommandRequest& originalCmd, int opIndex) {
    auto op = originalCmd.getUpdates()[opIndex];
    auto sampleId = op.getSampleId();
    invariant(sampleId);

    write_ops::UpdateCommandRequest sampledCmd(originalCmd.getNamespace(), {std::move(op)});
    sampledCmd.setLet(originalCmd.getLet());

    return {*sampleId,
            sampledCmd.getNamespace(),
            sampledCmd.toBSON(BSON("$db" << sampledCmd.getNamespace().db().toString()))};
}

/*
 * Returns a sampled delete command for the delete at 'opIndex' in the given delete command.
 */
SampledWriteCommandRequest makeSampledDeleteCommandRequest(
    const write_ops::DeleteCommandRequest& originalCmd, int opIndex) {
    auto op = originalCmd.getDeletes()[opIndex];
    auto sampleId = op.getSampleId();
    invariant(sampleId);

    write_ops::DeleteCommandRequest sampledCmd(originalCmd.getNamespace(), {std::move(op)});
    sampledCmd.setLet(originalCmd.getLet());

    return {*sampleId,
            sampledCmd.getNamespace(),
            sampledCmd.toBSON(BSON("$db" << sampledCmd.getNamespace().db().toString()))};
}

/*
 * Returns a sampled findAndModify command for the given findAndModify command.
 */
SampledWriteCommandRequest makeSampledFindAndModifyCommandRequest(
    const write_ops::FindAndModifyCommandRequest& originalCmd) {
    invariant(originalCmd.getSampleId());

    write_ops::FindAndModifyCommandRequest sampledCmd(originalCmd.getNamespace());
    sampledCmd.setQuery(originalCmd.getQuery());
    sampledCmd.setUpdate(originalCmd.getUpdate());
    sampledCmd.setRemove(originalCmd.getRemove());
    sampledCmd.setUpsert(originalCmd.getUpsert());
    sampledCmd.setNew(originalCmd.getNew());
    sampledCmd.setSort(originalCmd.getSort());
    sampledCmd.setCollation(originalCmd.getCollation());
    sampledCmd.setArrayFilters(originalCmd.getArrayFilters());
    sampledCmd.setLet(originalCmd.getLet());
    sampledCmd.setSampleId(originalCmd.getSampleId());

    return {*sampledCmd.getSampleId(),
            sampledCmd.getNamespace(),
            sampledCmd.toBSON(BSON("$db" << sampledCmd.getNamespace().db().toString()))};
}

}  // namespace

QueryAnalysisWriter& QueryAnalysisWriter::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

QueryAnalysisWriter& QueryAnalysisWriter::get(ServiceContext* serviceContext) {
    invariant(analyze_shard_key::isFeatureFlagEnabledIgnoreFCV(),
              "Only support analyzing queries when the feature flag is enabled");
    invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer,
              "Only support analyzing queries on a sharded cluster");
    return getQueryAnalysisWriter(serviceContext);
}

void QueryAnalysisWriter::onStartup() {
    auto serviceContext = getQueryAnalysisWriter.owner(this);
    auto periodicRunner = serviceContext->getPeriodicRunner();
    invariant(periodicRunner);

    stdx::lock_guard<Latch> lk(_mutex);

    PeriodicRunner::PeriodicJob QueryWriterJob(
        "QueryAnalysisQueryWriter",
        [this](Client* client) {
            if (MONGO_unlikely(disableQueryAnalysisWriter.shouldFail())) {
                return;
            }
            auto opCtx = client->makeOperationContext();
            _flushQueries(opCtx.get());
        },
        Seconds(gQueryAnalysisWriterIntervalSecs));
    _periodicQueryWriter = periodicRunner->makeJob(std::move(QueryWriterJob));
    _periodicQueryWriter.start();

    PeriodicRunner::PeriodicJob diffWriterJob(
        "QueryAnalysisDiffWriter",
        [this](Client* client) {
            if (MONGO_unlikely(disableQueryAnalysisWriter.shouldFail())) {
                return;
            }
            auto opCtx = client->makeOperationContext();
            _flushDiffs(opCtx.get());
        },
        Seconds(gQueryAnalysisWriterIntervalSecs));
    _periodicDiffWriter = periodicRunner->makeJob(std::move(diffWriterJob));
    _periodicDiffWriter.start();

    ThreadPool::Options threadPoolOptions;
    threadPoolOptions.maxThreads = gQueryAnalysisWriterMaxThreadPoolSize;
    threadPoolOptions.minThreads = gQueryAnalysisWriterMinThreadPoolSize;
    threadPoolOptions.threadNamePrefix = "QueryAnalysisWriter-";
    threadPoolOptions.poolName = "QueryAnalysisWriterThreadPool";
    threadPoolOptions.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };
    _executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(threadPoolOptions),
        executor::makeNetworkInterface("QueryAnalysisWriterNetwork"));
    _executor->startup();
}

void QueryAnalysisWriter::onShutdown() {
    if (_executor) {
        _executor->shutdown();
        _executor->join();
    }
    if (_periodicQueryWriter.isValid()) {
        _periodicQueryWriter.stop();
    }
    if (_periodicDiffWriter.isValid()) {
        _periodicDiffWriter.stop();
    }
}

void QueryAnalysisWriter::_flushQueries(OperationContext* opCtx) {
    try {
        _flush(opCtx, NamespaceString::kConfigSampledQueriesNamespace, &_queries);
    } catch (DBException& ex) {
        LOGV2(7047300,
              "Failed to flush queries, will try again at the next interval",
              "error"_attr = redact(ex));
    }
}

void QueryAnalysisWriter::_flushDiffs(OperationContext* opCtx) {
    try {
        _flush(opCtx, NamespaceString::kConfigSampledQueriesDiffNamespace, &_diffs);
    } catch (DBException& ex) {
        LOGV2(7075400,
              "Failed to flush diffs, will try again at the next interval",
              "error"_attr = redact(ex));
    }
}

void QueryAnalysisWriter::_flush(OperationContext* opCtx,
                                 const NamespaceString& ns,
                                 Buffer* buffer) {
    Buffer tmpBuffer;
    // The indices of invalid documents, e.g. documents that fail to insert with DuplicateKey errors
    // (i.e. duplicates) and BadValue errors. Such documents should not get added back to the buffer
    // when the inserts below fail.
    std::set<int> invalid;
    {
        stdx::lock_guard<Latch> lk(_mutex);
        if (buffer->isEmpty()) {
            return;
        }
        std::swap(tmpBuffer, *buffer);
    }
    ScopeGuard backSwapper([&] {
        stdx::lock_guard<Latch> lk(_mutex);
        for (int i = 0; i < tmpBuffer.getCount(); i++) {
            if (invalid.find(i) == invalid.end()) {
                buffer->add(tmpBuffer.at(i));
            }
        }
    });

    LOGV2_DEBUG(6876101, 2, "Persisting sampled queries", "count"_attr = tmpBuffer.getCount());

    // Insert the documents in batches from the back of the buffer so that we don't need to move the
    // documents forward after each batch.
    size_t baseIndex = tmpBuffer.getCount() - 1;
    size_t maxBatchSize = gQueryAnalysisWriterMaxBatchSize.load();

    while (!tmpBuffer.isEmpty()) {
        std::vector<BSONObj> docsToInsert;
        long long objSize = 0;

        size_t lastIndex = tmpBuffer.getCount();  // inclusive
        while (lastIndex > 0 && docsToInsert.size() < maxBatchSize) {
            // Check if the next document can fit in the batch.
            auto doc = tmpBuffer.at(lastIndex - 1);
            if (doc.objsize() + objSize >= kMaxBSONObjSizePerInsertBatch) {
                break;
            }
            lastIndex--;
            objSize += doc.objsize();
            docsToInsert.push_back(std::move(doc));
        }
        // We don't add a document that is above the size limit to the buffer so we should have
        // added at least one document to 'docsToInsert'.
        invariant(!docsToInsert.empty());

        insertDocuments(opCtx, ns, docsToInsert, [&](const BatchedCommandResponse& response) {
            if (response.isErrDetailsSet() && response.sizeErrDetails() > 0) {
                boost::optional<write_ops::WriteError> firstWriteErr;

                for (const auto& err : response.getErrDetails()) {
                    if (err.getStatus() == ErrorCodes::DuplicateKey ||
                        err.getStatus() == ErrorCodes::BadValue) {
                        LOGV2(7075402,
                              "Ignoring insert error",
                              "error"_attr = redact(err.getStatus()));
                        invalid.insert(baseIndex - err.getIndex());
                        continue;
                    }
                    if (!firstWriteErr) {
                        // Save the error for later. Go through the rest of the errors to see if
                        // there are any invalid documents so they can be discarded from the buffer.
                        firstWriteErr.emplace(err);
                    }
                }
                if (firstWriteErr) {
                    uassertStatusOK(firstWriteErr->getStatus());
                }
            } else {
                uassertStatusOK(response.toStatus());
            }
        });

        tmpBuffer.truncate(lastIndex, objSize);
        baseIndex -= lastIndex;
    }

    backSwapper.dismiss();
}

void QueryAnalysisWriter::Buffer::add(BSONObj doc) {
    if (doc.objsize() > kMaxBSONObjSizePerInsertBatch) {
        return;
    }
    _docs.push_back(std::move(doc));
    _numBytes += _docs.back().objsize();
}

void QueryAnalysisWriter::Buffer::truncate(size_t index, long long numBytes) {
    invariant(index >= 0);
    invariant(index < _docs.size());
    invariant(numBytes > 0);
    invariant(numBytes <= _numBytes);
    _docs.resize(index);
    _numBytes -= numBytes;
}

bool QueryAnalysisWriter::_exceedsMaxSizeBytes() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _queries.getSize() + _diffs.getSize() >= gQueryAnalysisWriterMaxMemoryUsageBytes.load();
}

ExecutorFuture<void> QueryAnalysisWriter::addFindQuery(const UUID& sampleId,
                                                       const NamespaceString& nss,
                                                       const BSONObj& filter,
                                                       const BSONObj& collation) {
    return _addReadQuery(sampleId, nss, SampledCommandNameEnum::kFind, filter, collation);
}

ExecutorFuture<void> QueryAnalysisWriter::addCountQuery(const UUID& sampleId,
                                                        const NamespaceString& nss,
                                                        const BSONObj& filter,
                                                        const BSONObj& collation) {
    return _addReadQuery(sampleId, nss, SampledCommandNameEnum::kCount, filter, collation);
}

ExecutorFuture<void> QueryAnalysisWriter::addDistinctQuery(const UUID& sampleId,
                                                           const NamespaceString& nss,
                                                           const BSONObj& filter,
                                                           const BSONObj& collation) {
    return _addReadQuery(sampleId, nss, SampledCommandNameEnum::kDistinct, filter, collation);
}

ExecutorFuture<void> QueryAnalysisWriter::addAggregateQuery(const UUID& sampleId,
                                                            const NamespaceString& nss,
                                                            const BSONObj& filter,
                                                            const BSONObj& collation) {
    return _addReadQuery(sampleId, nss, SampledCommandNameEnum::kAggregate, filter, collation);
}

ExecutorFuture<void> QueryAnalysisWriter::_addReadQuery(const UUID& sampleId,
                                                        const NamespaceString& nss,
                                                        SampledCommandNameEnum cmdName,
                                                        const BSONObj& filter,
                                                        const BSONObj& collation) {
    invariant(_executor);
    return ExecutorFuture<void>(_executor)
        .then([this,
               sampleId,
               nss,
               cmdName,
               filter = filter.getOwned(),
               collation = collation.getOwned()] {
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();

            auto collUuid = CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, nss);

            if (!collUuid) {
                LOGV2(7047301, "Found a sampled read query for non-existing collection");
                return;
            }

            auto cmd = SampledReadCommand{filter.getOwned(), collation.getOwned()};
            auto doc = SampledQueryDocument{sampleId, nss, *collUuid, cmdName, cmd.toBSON()};
            stdx::lock_guard<Latch> lk(_mutex);
            _queries.add(doc.toBSON());
        })
        .then([this] {
            if (_exceedsMaxSizeBytes()) {
                auto opCtxHolder = cc().makeOperationContext();
                auto opCtx = opCtxHolder.get();
                _flushQueries(opCtx);
            }
        })
        .onError([this, nss](Status status) {
            LOGV2(7047302,
                  "Failed to add read query",
                  "ns"_attr = nss,
                  "error"_attr = redact(status));
        });
}

ExecutorFuture<void> QueryAnalysisWriter::addUpdateQuery(
    const write_ops::UpdateCommandRequest& updateCmd, int opIndex) {
    invariant(updateCmd.getUpdates()[opIndex].getSampleId());
    invariant(_executor);

    return ExecutorFuture<void>(_executor)
        .then([this, sampledUpdateCmd = makeSampledUpdateCommandRequest(updateCmd, opIndex)]() {
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();

            auto collUuid =
                CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, sampledUpdateCmd.nss);

            if (!collUuid) {
                LOGV2_WARNING(7075300,
                              "Found a sampled update query for a non-existing collection");
                return;
            }

            auto doc = SampledQueryDocument{sampledUpdateCmd.sampleId,
                                            sampledUpdateCmd.nss,
                                            *collUuid,
                                            SampledCommandNameEnum::kUpdate,
                                            std::move(sampledUpdateCmd.cmd)};
            stdx::lock_guard<Latch> lk(_mutex);
            _queries.add(doc.toBSON());
        })
        .then([this] {
            if (_exceedsMaxSizeBytes()) {
                auto opCtxHolder = cc().makeOperationContext();
                auto opCtx = opCtxHolder.get();
                _flushQueries(opCtx);
            }
        })
        .onError([this, nss = updateCmd.getNamespace()](Status status) {
            LOGV2(7075301,
                  "Failed to add update query",
                  "ns"_attr = nss,
                  "error"_attr = redact(status));
        });
}

ExecutorFuture<void> QueryAnalysisWriter::addDeleteQuery(
    const write_ops::DeleteCommandRequest& deleteCmd, int opIndex) {
    invariant(deleteCmd.getDeletes()[opIndex].getSampleId());
    invariant(_executor);

    return ExecutorFuture<void>(_executor)
        .then([this, sampledDeleteCmd = makeSampledDeleteCommandRequest(deleteCmd, opIndex)]() {
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();

            auto collUuid =
                CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, sampledDeleteCmd.nss);

            if (!collUuid) {
                LOGV2_WARNING(7075302,
                              "Found a sampled delete query for a non-existing collection");
                return;
            }

            auto doc = SampledQueryDocument{sampledDeleteCmd.sampleId,
                                            sampledDeleteCmd.nss,
                                            *collUuid,
                                            SampledCommandNameEnum::kDelete,
                                            std::move(sampledDeleteCmd.cmd)};
            stdx::lock_guard<Latch> lk(_mutex);
            _queries.add(doc.toBSON());
        })
        .then([this] {
            if (_exceedsMaxSizeBytes()) {
                auto opCtxHolder = cc().makeOperationContext();
                auto opCtx = opCtxHolder.get();
                _flushQueries(opCtx);
            }
        })
        .onError([this, nss = deleteCmd.getNamespace()](Status status) {
            LOGV2(7075303,
                  "Failed to add delete query",
                  "ns"_attr = nss,
                  "error"_attr = redact(status));
        });
}

ExecutorFuture<void> QueryAnalysisWriter::addFindAndModifyQuery(
    const write_ops::FindAndModifyCommandRequest& findAndModifyCmd) {
    invariant(findAndModifyCmd.getSampleId());
    invariant(_executor);

    return ExecutorFuture<void>(_executor)
        .then([this,
               sampledFindAndModifyCmd =
                   makeSampledFindAndModifyCommandRequest(findAndModifyCmd)]() {
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();

            auto collUuid =
                CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, sampledFindAndModifyCmd.nss);

            if (!collUuid) {
                LOGV2_WARNING(7075304,
                              "Found a sampled findAndModify query for a non-existing collection");
                return;
            }

            auto doc = SampledQueryDocument{sampledFindAndModifyCmd.sampleId,
                                            sampledFindAndModifyCmd.nss,
                                            *collUuid,
                                            SampledCommandNameEnum::kFindAndModify,
                                            std::move(sampledFindAndModifyCmd.cmd)};
            stdx::lock_guard<Latch> lk(_mutex);
            _queries.add(doc.toBSON());
        })
        .then([this] {
            if (_exceedsMaxSizeBytes()) {
                auto opCtxHolder = cc().makeOperationContext();
                auto opCtx = opCtxHolder.get();
                _flushQueries(opCtx);
            }
        })
        .onError([this, nss = findAndModifyCmd.getNamespace()](Status status) {
            LOGV2(7075305,
                  "Failed to add findAndModify query",
                  "ns"_attr = nss,
                  "error"_attr = redact(status));
        });
}

ExecutorFuture<void> QueryAnalysisWriter::addDiff(const UUID& sampleId,
                                                  const NamespaceString& nss,
                                                  const UUID& collUuid,
                                                  const BSONObj& preImage,
                                                  const BSONObj& postImage) {
    invariant(_executor);
    return ExecutorFuture<void>(_executor)
        .then([this,
               sampleId,
               nss,
               collUuid,
               preImage = preImage.getOwned(),
               postImage = postImage.getOwned()]() {
            auto diff = doc_diff::computeInlineDiff(preImage, postImage);

            if (!diff || diff->isEmpty()) {
                return;
            }

            auto doc = SampledQueryDiffDocument{sampleId, nss, collUuid, std::move(*diff)};
            stdx::lock_guard<Latch> lk(_mutex);
            _diffs.add(doc.toBSON());
        })
        .then([this] {
            if (_exceedsMaxSizeBytes()) {
                auto opCtxHolder = cc().makeOperationContext();
                auto opCtx = opCtxHolder.get();
                _flushDiffs(opCtx);
            }
        })
        .onError([this, nss](Status status) {
            LOGV2(7075401, "Failed to add diff", "ns"_attr = nss, "error"_attr = redact(status));
        });
}

}  // namespace analyze_shard_key
}  // namespace mongo
