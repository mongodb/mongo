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

#include "mongo/bson/bsonobj.h"
#include "mongo/client/connpool.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/analyze_shard_key_server_parameters_gen.h"
#include "mongo/s/analyze_shard_key_util.h"
#include "mongo/s/query_analysis_sample_tracker.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future_util.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace analyze_shard_key {

namespace {

const auto getQueryAnalysisWriter = ServiceContext::declareDecoration<QueryAnalysisWriter>();

static ReplicaSetAwareServiceRegistry::Registerer<QueryAnalysisWriter>
    queryAnalysisWriterServiceRegisterer("QueryAnalysisWriter");

MONGO_FAIL_POINT_DEFINE(disableQueryAnalysisWriter);
MONGO_FAIL_POINT_DEFINE(hangQueryAnalysisWriterBeforeWritingLocally);
MONGO_FAIL_POINT_DEFINE(hangQueryAnalysisWriterBeforeWritingRemotely);

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

/**
 * Creates TTL index for the collection storing sampled queries.
 */
BSONObj createSampledQueriesTTLIndex(OperationContext* opCtx) {
    BSONObj resObj;

    DBDirectClient client(opCtx);
    client.runCommand(NamespaceString::kConfigSampledQueriesNamespace.db(),
                      BSON("createIndexes"
                           << NamespaceString::kConfigSampledQueriesNamespace.coll().toString()
                           << "indexes"
                           << BSON_ARRAY(QueryAnalysisWriter::kSampledQueriesTTLIndexSpec)),
                      resObj);

    LOGV2_DEBUG(7078401,
                1,
                "Creation of the TTL index for the collection storing sampled queries",
                logAttrs(NamespaceString::kConfigSampledQueriesNamespace),
                "response"_attr = redact(resObj));

    return resObj;
}

/**
 * Creates TTL index for the collection storing sampled diffs.
 */
BSONObj createSampledQueriesDiffTTLIndex(OperationContext* opCtx) {
    BSONObj resObj;

    DBDirectClient client(opCtx);
    client.runCommand(NamespaceString::kConfigSampledQueriesDiffNamespace.db(),
                      BSON("createIndexes"
                           << NamespaceString::kConfigSampledQueriesDiffNamespace.coll().toString()
                           << "indexes"
                           << BSON_ARRAY(QueryAnalysisWriter::kSampledQueriesDiffTTLIndexSpec)),
                      resObj);

    LOGV2_DEBUG(7078402,
                1,
                "Creation of the TTL index for the collection storing sampled diffs",
                logAttrs(NamespaceString::kConfigSampledQueriesDiffNamespace),
                "response"_attr = redact(resObj));

    return resObj;
}

struct SampledCommandRequest {
    UUID sampleId;
    NamespaceString nss;
    // The BSON for a SampledReadCommand or {Update,Delete,FindAndModify}CommandRequest.
    BSONObj cmd;
};

/*
 * Returns a sampled read command for a read with the given filter, collation, let and runtime
 * constants.
 */
SampledCommandRequest makeSampledReadCommand(const UUID& sampleId,
                                             const NamespaceString& nss,
                                             const BSONObj& filter,
                                             const BSONObj& collation,
                                             const boost::optional<BSONObj>& letParameters) {
    SampledReadCommand sampledCmd(filter, collation);
    sampledCmd.setLet(letParameters);
    return {sampleId, nss, sampledCmd.toBSON()};
}

/*
 * Returns a sampled update command for the update at 'opIndex' in the given update command.
 */
SampledCommandRequest makeSampledUpdateCommandRequest(
    const UUID& sampleId, const write_ops::UpdateCommandRequest& originalCmd, int opIndex) {
    auto op = originalCmd.getUpdates()[opIndex];
    if (op.getSampleId()) {
        tassert(ErrorCodes::IllegalOperation,
                "Cannot overwrite the existing sample id for the update query",
                op.getSampleId() == sampleId);
    } else {
        op.setSampleId(sampleId);
    }
    // If the initial query was a write without shard key, the two phase write protocol modifies the
    // query in the write phase. In order to get correct metrics, we need to reconstruct the
    // original query here.
    if (originalCmd.getOriginalQuery()) {
        tassert(7406500,
                "Found a _clusterWithoutShardKey command with batch size > 1",
                originalCmd.getUpdates().size() == 1);
        op.setQ(*originalCmd.getOriginalQuery());
        op.setCollation(originalCmd.getOriginalCollation());
    }

    write_ops::UpdateCommandRequest sampledCmd(originalCmd.getNamespace(), {std::move(op)});
    sampledCmd.setLet(originalCmd.getLet());

    return {sampleId,
            sampledCmd.getNamespace(),
            sampledCmd.toBSON(BSON("$db" << sampledCmd.getNamespace().db().toString()))};
}

/*
 * Returns a sampled delete command for the delete at 'opIndex' in the given delete command.
 */
SampledCommandRequest makeSampledDeleteCommandRequest(
    const UUID& sampleId, const write_ops::DeleteCommandRequest& originalCmd, int opIndex) {
    auto op = originalCmd.getDeletes()[opIndex];
    if (op.getSampleId()) {
        tassert(ErrorCodes::IllegalOperation,
                "Cannot overwrite the existing sample id for the delete query",
                op.getSampleId() == sampleId);
    } else {
        op.setSampleId(sampleId);
    }
    // If the initial query was a write without shard key, the two phase write protocol modifies the
    // query in the write phase. In order to get correct metrics, we need to reconstruct the
    // original query here.
    if (originalCmd.getOriginalQuery()) {
        tassert(7406501,
                "Found a _clusterWithoutShardKey command with batch size > 1",
                originalCmd.getDeletes().size() == 1);
        op.setQ(*originalCmd.getOriginalQuery());
        op.setCollation(originalCmd.getOriginalCollation());
    }

    write_ops::DeleteCommandRequest sampledCmd(originalCmd.getNamespace(), {std::move(op)});
    sampledCmd.setLet(originalCmd.getLet());

    return {sampleId,
            sampledCmd.getNamespace(),
            sampledCmd.toBSON(BSON("$db" << sampledCmd.getNamespace().db().toString()))};
}

/*
 * Returns a sampled findAndModify command for the given findAndModify command.
 */
SampledCommandRequest makeSampledFindAndModifyCommandRequest(
    const UUID& sampleId, const write_ops::FindAndModifyCommandRequest& originalCmd) {
    write_ops::FindAndModifyCommandRequest sampledCmd(originalCmd.getNamespace());
    if (sampledCmd.getSampleId()) {
        tassert(ErrorCodes::IllegalOperation,
                "Cannot overwrite the existing sample id for the findAndModify query",
                sampledCmd.getSampleId() == sampleId);
    } else {
        sampledCmd.setSampleId(sampleId);
    }
    // If the initial query was a write without shard key, the two phase write protocol modifies the
    // query in the write phase. In order to get correct metrics, we need to reconstruct the
    // original query here.
    if (originalCmd.getOriginalQuery()) {
        sampledCmd.setQuery(*originalCmd.getOriginalQuery());
        sampledCmd.setCollation(originalCmd.getOriginalCollation());
    } else {
        sampledCmd.setQuery(originalCmd.getQuery());
        sampledCmd.setCollation(originalCmd.getCollation());
    }
    sampledCmd.setUpdate(originalCmd.getUpdate());
    sampledCmd.setRemove(originalCmd.getRemove());
    sampledCmd.setUpsert(originalCmd.getUpsert());
    sampledCmd.setNew(originalCmd.getNew());
    sampledCmd.setSort(originalCmd.getSort());
    sampledCmd.setArrayFilters(originalCmd.getArrayFilters());
    sampledCmd.setLet(originalCmd.getLet());

    return {sampleId,
            sampledCmd.getNamespace(),
            sampledCmd.toBSON(BSON("$db" << sampledCmd.getNamespace().db().toString()))};
}

}  // namespace

const std::string QueryAnalysisWriter::kSampledQueriesTTLIndexName = "SampledQueriesTTLIndex";
const std::string QueryAnalysisWriter::kSampledQueriesDiffTTLIndexName =
    "SampledQueriesDiffTTLIndex";
BSONObj QueryAnalysisWriter::kSampledQueriesTTLIndexSpec(BSON("key"
                                                              << BSON("expireAt" << 1)
                                                              << "expireAfterSeconds" << 0 << "name"
                                                              << kSampledQueriesTTLIndexName));
BSONObj QueryAnalysisWriter::kSampledQueriesDiffTTLIndexSpec(
    BSON("key" << BSON("expireAt" << 1) << "expireAfterSeconds" << 0 << "name"
               << kSampledQueriesDiffTTLIndexName));

QueryAnalysisWriter* QueryAnalysisWriter::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

QueryAnalysisWriter* QueryAnalysisWriter::get(ServiceContext* serviceContext) {
    return &getQueryAnalysisWriter(serviceContext);
}

bool QueryAnalysisWriter::shouldRegisterReplicaSetAwareService() const {
    // This is invoked when the Register above is constructed which is before FCV is set so we need
    // to ignore FCV when checking if the feature flag is enabled.
    return supportsPersistingSampledQueries(true /* isReplEnabled */, true /* ignoreFCV */);
}

void QueryAnalysisWriter::onStartup(OperationContext* opCtx) {
    auto serviceContext = getQueryAnalysisWriter.owner(this);
    auto periodicRunner = serviceContext->getPeriodicRunner();
    invariant(periodicRunner);

    stdx::lock_guard<Latch> lk(_mutex);

    PeriodicRunner::PeriodicJob queryWriterJob(
        "QueryAnalysisQueryWriter",
        [this](Client* client) {
            if (MONGO_unlikely(disableQueryAnalysisWriter.shouldFail())) {
                return;
            }
            auto opCtx = client->makeOperationContext();
            _flushQueries(opCtx.get());
        },
        Seconds(gQueryAnalysisWriterIntervalSecs));
    _periodicQueryWriter = periodicRunner->makeJob(std::move(queryWriterJob));
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

void QueryAnalysisWriter::onStepUpComplete(OperationContext* opCtx, long long term) {
    createTTLIndexes(opCtx).getAsync([](auto) {});
}

ExecutorFuture<void> QueryAnalysisWriter::createTTLIndexes(OperationContext* opCtx) {
    static unsigned int tryCount = 0;
    invariant(_executor);
    auto future =
        AsyncTry([this, opCtx] {
            ++tryCount;

            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();

            auto status = getStatusFromCommandResult(createSampledQueriesTTLIndex(opCtx));
            if (!status.isOK() && status != ErrorCodes::IndexAlreadyExists) {
                if (tryCount % 100 == 0) {
                    LOGV2_WARNING(7078404,
                                  "Still retrying to create sampled queries TTL index; "
                                  "please create an index on {namespace} with specification "
                                  "{specification}.",
                                  logAttrs(NamespaceString::kConfigSampledQueriesNamespace),
                                  "specification"_attr =
                                      QueryAnalysisWriter::kSampledQueriesTTLIndexSpec,
                                  "tries"_attr = tryCount);
                }
                return status;
            }

            status = getStatusFromCommandResult(createSampledQueriesDiffTTLIndex(opCtx));
            if (!status.isOK() && status != ErrorCodes::IndexAlreadyExists) {
                if (tryCount % 100 == 0) {
                    LOGV2_WARNING(7078405,
                                  "Still retrying to create sampled queries diff TTL index; "
                                  "please create an index on {namespace} with specification "
                                  "{specification}.",
                                  logAttrs(NamespaceString::kConfigSampledQueriesDiffNamespace),
                                  "specification"_attr =
                                      QueryAnalysisWriter::kSampledQueriesDiffTTLIndexSpec,
                                  "tries"_attr = tryCount);
                }
                return status;
            }

            return Status::OK();
        })
            .until([](Status status) {
                // Stop retrying if index creation succeeds, or if server is no longer
                // primary.
                return (status.isOK() || ErrorCodes::isNotPrimaryError(status));
            })
            .withBackoffBetweenIterations(kExponentialBackoff)
            .on(_executor, CancellationToken::uncancelable());
    return future;
}

void QueryAnalysisWriter::_flushQueries(OperationContext* opCtx) {
    try {
        _flush(opCtx, &_queries);
    } catch (DBException& ex) {
        LOGV2(7047300,
              "Failed to flush queries, will try again at the next interval",
              "error"_attr = redact(ex));
    }
}

void QueryAnalysisWriter::_flushDiffs(OperationContext* opCtx) {
    try {
        _flush(opCtx, &_diffs);
    } catch (DBException& ex) {
        LOGV2(7075400,
              "Failed to flush diffs, will try again at the next interval",
              "error"_attr = redact(ex));
    }
}

void QueryAnalysisWriter::_flush(OperationContext* opCtx, Buffer* buffer) {
    const auto nss = buffer->getNss();

    Buffer tmpBuffer(nss);
    // The indices of invalid documents, e.g. documents that fail to insert with DuplicateKey errors
    // (i.e. duplicates) and BadValue errors. Such documents should not get added back to the buffer
    // when the inserts below fail.
    std::set<int> invalid;
    {
        stdx::lock_guard<Latch> lk(_mutex);
        if (buffer->isEmpty()) {
            return;
        }

        LOGV2_DEBUG(7372300,
                    1,
                    "About to flush the sample buffer",
                    logAttrs(nss),
                    "numDocs"_attr = buffer->getCount());

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
        LOGV2_DEBUG(
            6876102, 2, "Persisting samples", logAttrs(nss), "numDocs"_attr = docsToInsert.size());

        insertDocuments(opCtx, nss, docsToInsert, [&](const BSONObj& resObj) {
            BatchedCommandResponse res;
            std::string errMsg;

            if (!res.parseBSON(resObj, &errMsg)) {
                uasserted(ErrorCodes::FailedToParse, errMsg);
            }

            if (res.isErrDetailsSet() && res.sizeErrDetails() > 0) {
                boost::optional<write_ops::WriteError> firstWriteErr;

                for (const auto& err : res.getErrDetails()) {
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
                uassertStatusOK(res.toStatus());
            }
        });

        tmpBuffer.truncate(lastIndex, objSize);
        baseIndex -= lastIndex;
    }

    backSwapper.dismiss();
}

bool QueryAnalysisWriter::Buffer::add(BSONObj doc) {
    if (doc.objsize() > kMaxBSONObjSizePerInsertBatch) {
        LOGV2_DEBUG(7372301,
                    4,
                    "Ignoring a sample due to its size",
                    logAttrs(_nss),
                    "size"_attr = doc.objsize(),
                    "doc"_attr = redact(doc));
        return false;
    }

    LOGV2_DEBUG(
        7372302, 4, "Adding a sample to the buffer", logAttrs(_nss), "doc"_attr = redact(doc));
    _docs.push_back(std::move(doc));
    _numBytes += _docs.back().objsize();
    return true;
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

ExecutorFuture<void> QueryAnalysisWriter::addFindQuery(
    const UUID& sampleId,
    const NamespaceString& nss,
    const BSONObj& filter,
    const BSONObj& collation,
    const boost::optional<BSONObj>& letParameters) {
    return _addReadQuery(
        sampleId, nss, SampledCommandNameEnum::kFind, filter, collation, letParameters);
}

ExecutorFuture<void> QueryAnalysisWriter::addAggregateQuery(
    const UUID& sampleId,
    const NamespaceString& nss,
    const BSONObj& filter,
    const BSONObj& collation,
    const boost::optional<BSONObj>& letParameters) {
    return _addReadQuery(
        sampleId, nss, SampledCommandNameEnum::kAggregate, filter, collation, letParameters);
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

ExecutorFuture<void> QueryAnalysisWriter::_addReadQuery(
    const UUID& sampleId,
    const NamespaceString& nss,
    SampledCommandNameEnum cmdName,
    const BSONObj& filter,
    const BSONObj& collation,
    const boost::optional<BSONObj>& letParameters) {
    invariant(_executor);
    return ExecutorFuture<void>(_executor)
        .then([this,
               cmdName,
               sampledReadCmd =
                   makeSampledReadCommand(sampleId, nss, filter, collation, letParameters)] {
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();

            auto collUuid =
                CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, sampledReadCmd.nss);
            if (!collUuid) {
                LOGV2(7047301, "Found a sampled read query for non-existing collection");
                return;
            }

            auto expireAt = opCtx->getServiceContext()->getFastClockSource()->now() +
                mongo::Milliseconds(gQueryAnalysisSampleExpirationSecs.load() * 1000);
            auto doc = SampledQueryDocument{sampledReadCmd.sampleId,
                                            sampledReadCmd.nss,
                                            *collUuid,
                                            cmdName,
                                            std::move(sampledReadCmd.cmd),
                                            expireAt}
                           .toBSON();

            stdx::lock_guard<Latch> lk(_mutex);
            if (_queries.add(doc)) {
                QueryAnalysisSampleTracker::get(opCtx).incrementReads(
                    opCtx, sampledReadCmd.nss, *collUuid, doc.objsize());
            }
        })
        .then([this] {
            if (_exceedsMaxSizeBytes()) {
                auto opCtxHolder = cc().makeOperationContext();
                auto opCtx = opCtxHolder.get();
                _flushQueries(opCtx);
            }
        })
        .onError([this, nss](Status status) {
            LOGV2(
                7047302, "Failed to add read query", logAttrs(nss), "error"_attr = redact(status));
        });
}

ExecutorFuture<void> QueryAnalysisWriter::addUpdateQuery(
    const UUID& sampleId, const write_ops::UpdateCommandRequest& updateCmd, int opIndex) {
    invariant(_executor);

    return ExecutorFuture<void>(_executor)
        .then([this,
               sampledUpdateCmd = makeSampledUpdateCommandRequest(sampleId, updateCmd, opIndex)]() {
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();

            auto collUuid =
                CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, sampledUpdateCmd.nss);
            if (!collUuid) {
                LOGV2_WARNING(7075300,
                              "Found a sampled update query for a non-existing collection");
                return;
            }

            auto expireAt = opCtx->getServiceContext()->getFastClockSource()->now() +
                mongo::Milliseconds(gQueryAnalysisSampleExpirationSecs.load() * 1000);
            auto doc = SampledQueryDocument{sampledUpdateCmd.sampleId,
                                            sampledUpdateCmd.nss,
                                            *collUuid,
                                            SampledCommandNameEnum::kUpdate,
                                            std::move(sampledUpdateCmd.cmd),
                                            expireAt}
                           .toBSON();

            stdx::lock_guard<Latch> lk(_mutex);
            if (_queries.add(doc)) {
                QueryAnalysisSampleTracker::get(opCtx).incrementWrites(
                    opCtx, sampledUpdateCmd.nss, *collUuid, doc.objsize());
            }
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
                  logAttrs(nss),
                  "error"_attr = redact(status));
        });
}

ExecutorFuture<void> QueryAnalysisWriter::addUpdateQuery(
    const write_ops::UpdateCommandRequest& updateCmd, int opIndex) {
    auto sampleId = updateCmd.getUpdates()[opIndex].getSampleId();
    invariant(sampleId);
    return addUpdateQuery(*sampleId, updateCmd, opIndex);
}

ExecutorFuture<void> QueryAnalysisWriter::addDeleteQuery(
    const UUID& sampleId, const write_ops::DeleteCommandRequest& deleteCmd, int opIndex) {
    invariant(_executor);

    return ExecutorFuture<void>(_executor)
        .then([this,
               sampledDeleteCmd = makeSampledDeleteCommandRequest(sampleId, deleteCmd, opIndex)]() {
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();

            auto collUuid =
                CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, sampledDeleteCmd.nss);
            if (!collUuid) {
                LOGV2_WARNING(7075302,
                              "Found a sampled delete query for a non-existing collection");
                return;
            }

            auto expireAt = opCtx->getServiceContext()->getFastClockSource()->now() +
                mongo::Milliseconds(gQueryAnalysisSampleExpirationSecs.load() * 1000);
            auto doc = SampledQueryDocument{sampledDeleteCmd.sampleId,
                                            sampledDeleteCmd.nss,
                                            *collUuid,
                                            SampledCommandNameEnum::kDelete,
                                            std::move(sampledDeleteCmd.cmd),
                                            expireAt}
                           .toBSON();

            stdx::lock_guard<Latch> lk(_mutex);
            if (_queries.add(doc)) {
                QueryAnalysisSampleTracker::get(opCtx).incrementWrites(
                    opCtx, sampledDeleteCmd.nss, *collUuid, doc.objsize());
            }
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
                  logAttrs(nss),
                  "error"_attr = redact(status));
        });
}

ExecutorFuture<void> QueryAnalysisWriter::addDeleteQuery(
    const write_ops::DeleteCommandRequest& deleteCmd, int opIndex) {
    auto sampleId = deleteCmd.getDeletes()[opIndex].getSampleId();
    invariant(sampleId);
    return addDeleteQuery(*sampleId, deleteCmd, opIndex);
}

ExecutorFuture<void> QueryAnalysisWriter::addFindAndModifyQuery(
    const UUID& sampleId, const write_ops::FindAndModifyCommandRequest& findAndModifyCmd) {
    invariant(_executor);

    return ExecutorFuture<void>(_executor)
        .then([this,
               sampledFindAndModifyCmd =
                   makeSampledFindAndModifyCommandRequest(sampleId, findAndModifyCmd)]() {
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();

            auto collUuid =
                CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, sampledFindAndModifyCmd.nss);
            if (!collUuid) {
                LOGV2_WARNING(7075304,
                              "Found a sampled findAndModify query for a non-existing collection");
                return;
            }

            auto expireAt = opCtx->getServiceContext()->getFastClockSource()->now() +
                mongo::Milliseconds(gQueryAnalysisSampleExpirationSecs.load() * 1000);
            auto doc = SampledQueryDocument{sampledFindAndModifyCmd.sampleId,
                                            sampledFindAndModifyCmd.nss,
                                            *collUuid,
                                            SampledCommandNameEnum::kFindAndModify,
                                            std::move(sampledFindAndModifyCmd.cmd),
                                            expireAt}
                           .toBSON();

            stdx::lock_guard<Latch> lk(_mutex);
            if (_queries.add(doc)) {
                QueryAnalysisSampleTracker::get(opCtx).incrementWrites(
                    opCtx, sampledFindAndModifyCmd.nss, *collUuid, doc.objsize());
            }
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
                  logAttrs(nss),
                  "error"_attr = redact(status));
        });
}

ExecutorFuture<void> QueryAnalysisWriter::addFindAndModifyQuery(
    const write_ops::FindAndModifyCommandRequest& findAndModifyCmd) {
    auto sampleId = findAndModifyCmd.getSampleId();
    invariant(sampleId);
    return addFindAndModifyQuery(*sampleId, findAndModifyCmd);
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
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();

            if (!diff || diff->isEmpty()) {
                return;
            }

            auto expireAt = opCtx->getServiceContext()->getFastClockSource()->now() +
                mongo::Milliseconds(gQueryAnalysisSampleExpirationSecs.load() * 1000);
            auto doc =
                SampledQueryDiffDocument{sampleId, nss, collUuid, std::move(*diff), expireAt};

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
            LOGV2(7075401, "Failed to add diff", logAttrs(nss), "error"_attr = redact(status));
        });
}

}  // namespace analyze_shard_key
}  // namespace mongo
