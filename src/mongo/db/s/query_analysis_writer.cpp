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

#include "mongo/db/s/query_analysis_writer.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/s/analyze_shard_key_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/analyze_shard_key_role.h"
#include "mongo/s/analyze_shard_key_server_parameters_gen.h"
#include "mongo/s/query_analysis_client.h"
#include "mongo/s/query_analysis_sample_tracker.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"
#include "mongo/util/scoped_unlock.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <functional>
#include <set>
#include <tuple>
#include <utility>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace analyze_shard_key {

namespace {

using WriterIntervalSecs =
    decltype(QueryAnalysisWriter::observeQueryAnalysisWriterIntervalSecs)::Argument;

const auto getQueryAnalysisWriter = ServiceContext::declareDecoration<QueryAnalysisWriter>();

static ReplicaSetAwareServiceRegistry::Registerer<QueryAnalysisWriter>
    queryAnalysisWriterServiceRegisterer("QueryAnalysisWriter");

MONGO_FAIL_POINT_DEFINE(disableQueryAnalysisWriter);
MONGO_FAIL_POINT_DEFINE(disableQueryAnalysisWriterFlusher);
MONGO_FAIL_POINT_DEFINE(queryAnalysisWriterMockInsertCommandResponse);
MONGO_FAIL_POINT_DEFINE(queryAnalysisWriterSkipActiveSamplingCheck);

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

/**
 * Creates index with the requested specs for the given collection.
 */
BSONObj createIndex(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& indexSpec) {
    BSONObj resObj;

    DBDirectClient client(opCtx);
    client.runCommand(nss.dbName(),
                      BSON("createIndexes" << nss.coll() << "indexes" << BSON_ARRAY(indexSpec)),
                      resObj);

    LOGV2_DEBUG(7078401,
                1,
                "Finished running the command to create index",
                logAttrs(nss),
                "indexSpec"_attr = indexSpec,
                "response"_attr = redact(resObj));

    return resObj;
}

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
    OperationContext* opCtx,
    const UUID& sampleId,
    const write_ops::UpdateCommandRequest& originalCmd,
    int opIndex) {
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
    if (originalCmd.getOriginalQuery() || originalCmd.getOriginalCollation()) {
        tassert(7406500,
                "Found a _clusterWithoutShardKey command with batch size > 1",
                originalCmd.getUpdates().size() == 1);
        uassert(ErrorCodes::InvalidOptions,
                "Cannot specify '$_originalQuery' or '$_originalCollation' since they are internal "
                "fields",
                isInternalClient(opCtx));
        op.setQ(*originalCmd.getOriginalQuery());
        op.setCollation(originalCmd.getOriginalCollation());
    }

    write_ops::UpdateCommandRequest sampledCmd(originalCmd.getNamespace(), {std::move(op)});
    sampledCmd.setLet(originalCmd.getLet());
    sampledCmd.setBypassEmptyTsReplacement(originalCmd.getBypassEmptyTsReplacement());

    // "$db" is only included when serializing to OP_MSG, so we manually append it here.
    BSONObjBuilder bob(sampledCmd.toBSON());
    bob.append("$db",
               DatabaseNameUtil::serialize(sampledCmd.getNamespace().dbName(),
                                           SerializationContext::stateCommandRequest()));

    return {sampleId, sampledCmd.getNamespace(), bob.obj()};
}

/*
 * Returns a sampled delete command for the delete at 'opIndex' in the given delete command.
 */
SampledCommandRequest makeSampledDeleteCommandRequest(
    OperationContext* opCtx,
    const UUID& sampleId,
    const write_ops::DeleteCommandRequest& originalCmd,
    int opIndex) {
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
    if (originalCmd.getOriginalQuery() || originalCmd.getOriginalCollation()) {
        tassert(7406501,
                "Found a _clusterWithoutShardKey command with batch size > 1",
                originalCmd.getDeletes().size() == 1);
        uassert(ErrorCodes::InvalidOptions,
                "Cannot specify '$_originalQuery' or '$_originalCollation' since they are internal "
                "fields",
                isInternalClient(opCtx));
        op.setQ(*originalCmd.getOriginalQuery());
        op.setCollation(originalCmd.getOriginalCollation());
    }

    write_ops::DeleteCommandRequest sampledCmd(originalCmd.getNamespace(), {std::move(op)});
    sampledCmd.setLet(originalCmd.getLet());
    sampledCmd.setBypassEmptyTsReplacement(originalCmd.getBypassEmptyTsReplacement());

    // "$db" is only included when serializing to OP_MSG, so we manually append it here.
    BSONObjBuilder bob(sampledCmd.toBSON());
    bob.append("$db",
               DatabaseNameUtil::serialize(sampledCmd.getNamespace().dbName(),
                                           SerializationContext::stateCommandRequest()));
    return {sampleId, sampledCmd.getNamespace(), bob.obj()};
}

/*
 * Returns a sampled findAndModify command for the given findAndModify command.
 */
SampledCommandRequest makeSampledFindAndModifyCommandRequest(
    OperationContext* opCtx,
    const UUID& sampleId,
    const write_ops::FindAndModifyCommandRequest& originalCmd) {
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
    if (originalCmd.getOriginalQuery() || originalCmd.getOriginalCollation()) {
        uassert(ErrorCodes::InvalidOptions,
                "Cannot specify '$_originalQuery' or '$_originalCollation' since they are internal "
                "fields",
                isInternalClient(opCtx));
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
    sampledCmd.setBypassEmptyTsReplacement(originalCmd.getBypassEmptyTsReplacement());

    // "$db" is only included when serializing to OP_MSG, so we manually append it here.
    BSONObjBuilder bob(sampledCmd.toBSON());
    bob.append("$db", sampledCmd.getNamespace().db_forSharding());
    return {sampleId, sampledCmd.getNamespace(), bob.obj()};
}

/*
 * Returns true if a sample for the collection with the given namespace and collection uuid should
 * be persisted. If the collection does not exist (i.e. the collection uuid is none), returns false.
 * If the collection has been recreated or renamed (i.e. the given collection uuid does not match
 * the one in the sampling configuration), returns false. Otherwise, returns true.
 */
bool shouldPersistSample(OperationContext* opCtx,
                         const NamespaceString& nss,
                         const boost::optional<UUID>& collUuid) {
    if (!collUuid) {
        return false;
    }
    return MONGO_unlikely(queryAnalysisWriterSkipActiveSamplingCheck.shouldFail()) ||
        QueryAnalysisSampleTracker::get(opCtx).isSamplingActive(nss, *collUuid);
}

/**
 * Returns true if the writer should not retry inserting the document(s) that failed with the
 * given error again.
 */
bool isNonRetryableInsertError(const ErrorCodes::Error& errorCode) {
    return QueryAnalysisWriter::kNonRetryableInsertErrorCodes.find(errorCode) !=
        QueryAnalysisWriter::kNonRetryableInsertErrorCodes.end();
}

/**
 * Inserts the documents in buffer into the collection it is associated with in batches. Also remove
 * succesful documents from the buffer and also keeps track of indexes to invalid entries.
 */
void flushBuffer(OperationContext* opCtx,
                 QueryAnalysisWriter::Buffer* tmpBuffer,
                 std::set<int>* invalidDocIndexes) {
    const auto nss = tmpBuffer->getNss();

    // Insert the documents in batches from the back of the buffer so that we don't need to move the
    // documents forward after each batch.
    size_t baseIndex = tmpBuffer->getCount() - 1;
    size_t maxBatchSize = gQueryAnalysisWriterMaxBatchSize.load();

    while (!tmpBuffer->isEmpty()) {
        std::vector<BSONObj> docsToInsert;
        long long objSize = 0;

        size_t lastIndex = tmpBuffer->getCount();  // inclusive
        while (lastIndex > 0 && docsToInsert.size() < maxBatchSize) {
            // Check if the next document can fit in the batch.
            auto doc = tmpBuffer->at(lastIndex - 1);
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

        QueryAnalysisClient::get(opCtx).insert(
            opCtx, nss, docsToInsert, [&](const BSONObj& resObj) {
                BatchedCommandResponse res;
                std::string errMsg;

                if (!res.parseBSON(resObj, &errMsg)) {
                    uasserted(ErrorCodes::FailedToParse, errMsg);
                }

                queryAnalysisWriterMockInsertCommandResponse.executeIf(
                    [&](const BSONObj& data) {
                        if (data.hasField("errorDetails")) {
                            auto mockErrDetailsObj = data["errorDetails"].Obj();
                            res.addToErrDetails(write_ops::WriteError::parse(mockErrDetailsObj));
                        } else {
                            uasserted(9881700,
                                      str::stream() << "Expected the failpoint to specify "
                                                       "'errorDetails'"
                                                    << data);
                        }
                    },
                    [&](const BSONObj& data) {
                        for (const auto& doc : docsToInsert) {
                            auto docId = doc["_id"].wrap();
                            auto docIdToMatch = data["_id"].wrap();
                            if (docId.woCompare(docIdToMatch) == 0) {
                                return true;
                            }
                        }
                        return false;
                    });

                if (res.isErrDetailsSet() && res.sizeErrDetails() > 0) {
                    boost::optional<write_ops::WriteError> firstWriteErr;

                    for (const auto& err : res.getErrDetails()) {
                        if (isNonRetryableInsertError(err.getStatus().code())) {
                            int actualIndex = baseIndex - err.getIndex();
                            LOGV2(7075402,
                                  "Ignoring insert error",
                                  "actualIndex"_attr = actualIndex,
                                  "error"_attr = redact(err.getStatus()));
                            dassert(actualIndex >= 0,
                                    str::stream() << "Found an invalid index " << actualIndex);
                            dassert(actualIndex < tmpBuffer->getCount(),
                                    str::stream() << "Found an invalid index " << actualIndex);
                            invalidDocIndexes->insert(actualIndex);
                            continue;
                        }
                        if (!firstWriteErr) {
                            // Save the error for later. Go through the rest of the errors to see if
                            // there are any invalid documents so they can be discarded from the
                            // buffer.
                            firstWriteErr.emplace(err);
                        }
                    }
                    if (firstWriteErr) {
                        uassertStatusOK(firstWriteErr->getStatus());
                    }
                } else {
                    if (isNonRetryableInsertError(res.toStatus().code())) {
                        return;
                    }
                    uassertStatusOK(res.toStatus());
                }
            });

        tmpBuffer->truncate(lastIndex, objSize);
        baseIndex = tmpBuffer->getCount() - 1;
    }
}

}  // namespace

const std::string QueryAnalysisWriter::kSampledQueriesTTLIndexName = "SampledQueriesTTLIndex";
BSONObj QueryAnalysisWriter::kSampledQueriesTTLIndexSpec(
    BSON("key" << BSON(SampledQueryDocument::kExpireAtFieldName << 1) << "expireAfterSeconds" << 0
               << "name" << kSampledQueriesTTLIndexName));

const std::string QueryAnalysisWriter::kSampledQueriesDiffTTLIndexName =
    "SampledQueriesDiffTTLIndex";
BSONObj QueryAnalysisWriter::kSampledQueriesDiffTTLIndexSpec(
    BSON("key" << BSON(SampledQueryDiffDocument::kExpireAtFieldName << 1) << "expireAfterSeconds"
               << 0 << "name" << kSampledQueriesDiffTTLIndexName));

const std::string QueryAnalysisWriter::kAnalyzeShardKeySplitPointsTTLIndexName =
    "AnalyzeShardKeySplitPointsTTLIndex";
BSONObj QueryAnalysisWriter::kAnalyzeShardKeySplitPointsTTLIndexSpec(
    BSON("key" << BSON(AnalyzeShardKeySplitPointDocument::kExpireAtFieldName << 1)
               << "expireAfterSeconds" << 0 << "name" << kAnalyzeShardKeySplitPointsTTLIndexName));

const std::map<NamespaceString, BSONObj> QueryAnalysisWriter::kTTLIndexes = {
    {NamespaceString::kConfigSampledQueriesNamespace, kSampledQueriesTTLIndexSpec},
    {NamespaceString::kConfigSampledQueriesDiffNamespace, kSampledQueriesDiffTTLIndexSpec},
    {NamespaceString::kConfigAnalyzeShardKeySplitPointsNamespace,
     kAnalyzeShardKeySplitPointsTTLIndexSpec}};

// Do not retry upon getting an error indicating that the documents are invalid since the inserts
// are not going to succeed in the next try anyway.
const std::set<ErrorCodes::Error> QueryAnalysisWriter::kNonRetryableInsertErrorCodes = {
    ErrorCodes::BSONObjectTooLarge, ErrorCodes::BadValue, ErrorCodes::DuplicateKey};

QueryAnalysisWriter* QueryAnalysisWriter::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

QueryAnalysisWriter* QueryAnalysisWriter::get(ServiceContext* serviceContext) {
    return &getQueryAnalysisWriter(serviceContext);
}

bool QueryAnalysisWriter::shouldRegisterReplicaSetAwareService() const {
    return supportsPersistingSampledQueries(true /* isReplEnabled */);
}

void QueryAnalysisWriter::onStartup(OperationContext* opCtx) {
    if (MONGO_unlikely(disableQueryAnalysisWriter.shouldFail())) {
        return;
    }

    auto serviceContext = getQueryAnalysisWriter.owner(this);
    auto periodicRunner = serviceContext->getPeriodicRunner();
    invariant(periodicRunner);

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    PeriodicRunner::PeriodicJob queryWriterJob(
        "QueryAnalysisQueryWriter",
        [this](Client* client) {
            if (MONGO_unlikely(disableQueryAnalysisWriterFlusher.shouldFail())) {
                return;
            }
            auto opCtx = client->makeOperationContext();
            try {
                _flushQueries(opCtx.get());
            } catch (const DBException& e) {
                LOGV2_WARNING(
                    7466204,
                    "Query Analysis Query Writer encountered unexpected error, will retry after"
                    "gQueryAnalysisWriterIntervalSecs",
                    "error"_attr = e.toString());
            }
        },
        Seconds(gQueryAnalysisWriterIntervalSecs.load()),
        true /*isKillableByStepdown*/);
    _periodicQueryWriter =
        std::make_shared<PeriodicJobAnchor>(periodicRunner->makeJob(std::move(queryWriterJob)));
    _periodicQueryWriter->start();

    PeriodicRunner::PeriodicJob diffWriterJob(
        "QueryAnalysisDiffWriter",
        [this](Client* client) {
            if (MONGO_unlikely(disableQueryAnalysisWriterFlusher.shouldFail())) {
                return;
            }
            auto opCtx = client->makeOperationContext();
            try {
                _flushDiffs(opCtx.get());
            } catch (const DBException& e) {
                LOGV2_WARNING(7466201,
                              "Query Analysis Diff Writer encountered unexpected error, will retry "
                              "after gQueryAnalysisWriterIntervalSecs",
                              "error"_attr = e.toString());
            }
        },
        Seconds(gQueryAnalysisWriterIntervalSecs.load()),
        true /*isKillableByStepdown*/);
    _periodicDiffWriter =
        std::make_shared<PeriodicJobAnchor>(periodicRunner->makeJob(std::move(diffWriterJob)));
    _periodicDiffWriter->start();

    QueryAnalysisWriter::observeQueryAnalysisWriterIntervalSecs.addObserver(
        [queryWriter = _periodicQueryWriter,
         diffWriter = _periodicDiffWriter](const WriterIntervalSecs& secs) {
            try {
                queryWriter->setPeriod(Seconds(secs));
                diffWriter->setPeriod(Seconds(secs));
            } catch (const DBException& ex) {
                LOGV2(7891302,
                      "Failed to update the periods of the threads for writing sampled queries and "
                      "diffs to disk",
                      "error"_attr = ex.toStatus());
            }
        });

    ThreadPool::Options threadPoolOptions;
    threadPoolOptions.maxThreads = gQueryAnalysisWriterMaxThreadPoolSize;
    threadPoolOptions.minThreads = gQueryAnalysisWriterMinThreadPoolSize;
    threadPoolOptions.threadNamePrefix = "QueryAnalysisWriter-";
    threadPoolOptions.poolName = "QueryAnalysisWriterThreadPool";
    threadPoolOptions.onCreateThread = [service =
                                            opCtx->getService()](const std::string& threadName) {
        Client::initThread(threadName, service);
    };
    _executor = executor::ThreadPoolTaskExecutor::create(
        std::make_unique<ThreadPool>(threadPoolOptions),
        executor::makeNetworkInterface("QueryAnalysisWriterNetwork"));
    _executor->startup();
}

void QueryAnalysisWriter::onShutdown() {
    _isPrimary.store(false);
    if (_executor) {
        _executor->shutdown();
        _executor->join();
    }
    if (_periodicQueryWriter && _periodicQueryWriter->isValid()) {
        _periodicQueryWriter->stop();
    }
    if (_periodicDiffWriter && _periodicDiffWriter->isValid()) {
        _periodicDiffWriter->stop();
    }
}

void QueryAnalysisWriter::onStepUpComplete(OperationContext* opCtx, long long term) {
    if (MONGO_unlikely(disableQueryAnalysisWriter.shouldFail())) {
        return;
    }

    _isPrimary.store(true);
    createTTLIndexes(opCtx).getAsync([](auto) {});
}

ExecutorFuture<void> QueryAnalysisWriter::createTTLIndexes(OperationContext* opCtx) {
    invariant(_executor);

    static unsigned int tryCount = 0;

    auto future =
        AsyncTry([this] {
            ++tryCount;

            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();

            for (const auto& [nss, indexSpec] : kTTLIndexes) {
                auto status = getStatusFromCommandResult(createIndex(opCtx, nss, indexSpec));
                if (!status.isOK() && status != ErrorCodes::IndexAlreadyExists) {
                    if (tryCount % 100 == 0) {
                        LOGV2_WARNING(7078402,
                                      "Still retrying to create TTL index; "
                                      "please create an index on {namespace} with specification "
                                      "{specification}.",
                                      logAttrs(nss),
                                      "specification"_attr = indexSpec,
                                      "tries"_attr = tryCount,
                                      "isPrimary"_attr = _isPrimary.load());
                    }
                    return status;
                }
            }
            return Status::OK();
        })
            .until([this](Status status) {
                // Stop retrying if index creation succeeds, or if server is no longer
                // primary.
                return (status.isOK() ||
                        (ErrorCodes::isNotPrimaryError(status) && !_isPrimary.load()));
            })
            .withBackoffBetweenIterations(kExponentialBackoff)
            .on(_executor, CancellationToken::uncancelable());
    return future;
}

void QueryAnalysisWriter::_flushQueries(OperationContext* opCtx) {
    try {
        stdx::unique_lock lk(_mutex);
        _flush(opCtx, lk, &_queries);
    } catch (DBException& ex) {
        LOGV2(7047300,
              "Failed to flush queries, will try again at the next interval",
              "error"_attr = redact(ex));
    }
}

void QueryAnalysisWriter::_flushDiffs(OperationContext* opCtx) {
    try {
        stdx::unique_lock lk(_mutex);
        _flush(opCtx, lk, &_diffs);
    } catch (DBException& ex) {
        LOGV2(7075400,
              "Failed to flush diffs, will try again at the next interval",
              "error"_attr = redact(ex));
    }
}

void QueryAnalysisWriter::_flush(OperationContext* opCtx,
                                 stdx::unique_lock<stdx::mutex>& lk,
                                 Buffer* buffer) {
    invariant(lk.owns_lock());
    const auto nss = buffer->getNss();

    Buffer tmpBuffer(nss);
    // The indices of invalid documents, e.g. documents that fail to insert with DuplicateKey errors
    // (i.e. duplicates) and BadValue errors. Such documents should not get added back to the buffer
    // when the inserts below fail.
    std::set<int> invalidDocIndexes;
    if (buffer->isEmpty()) {
        return;
    }

    LOGV2_DEBUG(7372300,
                1,
                "About to flush the sample buffer",
                logAttrs(nss),
                "numDocs"_attr = buffer->getCount());

    std::swap(tmpBuffer, *buffer);

    ScopeGuard backSwapper([&] {
        for (int i = 0; i < tmpBuffer.getCount(); i++) {
            if (invalidDocIndexes.find(i) == invalidDocIndexes.end()) {
                buffer->add(tmpBuffer.at(i));
            }
        }
    });

    {
        ScopedUnlock unlockBlock(lk);
        flushBuffer(opCtx, &tmpBuffer, &invalidDocIndexes);
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
    stdx::lock_guard<stdx::mutex> lk(_mutex);
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
               sampledReadCmd = makeSampledReadCommand(
                   sampleId, nss, filter, collation, letParameters)]() mutable {
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();

            auto collUuid = getCollectionUUID(opCtx, sampledReadCmd.nss);
            if (!shouldPersistSample(opCtx, sampledReadCmd.nss, collUuid)) {
                return;
            }

            auto expireAt = opCtx->getServiceContext()->getFastClockSource()->now() +
                mongo::Seconds(gQueryAnalysisSampleExpirationSecs.load());
            auto doc = SampledQueryDocument{sampledReadCmd.sampleId,
                                            sampledReadCmd.nss,
                                            *collUuid,
                                            cmdName,
                                            std::move(sampledReadCmd.cmd),
                                            expireAt}
                           .toBSON();

            stdx::lock_guard<stdx::mutex> lk(_mutex);
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

ExecutorFuture<void> QueryAnalysisWriter::addUpdateQuery(SampledCommandNameEnum cmdName,
                                                         SampledCommandRequest sampledUpdateCmd) {
    invariant(_executor);
    invariant(cmdName == SampledCommandNameEnum::kUpdate ||
              cmdName == SampledCommandNameEnum::kBulkWrite);

    auto nss = sampledUpdateCmd.nss;

    return ExecutorFuture<void>(_executor)
        .then(
            [this, cmdName = std::move(cmdName), sampledUpdateCmd = std::move(sampledUpdateCmd)]() {
                auto opCtxHolder = cc().makeOperationContext();
                auto opCtx = opCtxHolder.get();

                auto collUuid = getCollectionUUID(opCtx, sampledUpdateCmd.nss);
                if (!shouldPersistSample(opCtx, sampledUpdateCmd.nss, collUuid)) {
                    return;
                }

                auto expireAt = opCtx->getServiceContext()->getFastClockSource()->now() +
                    mongo::Seconds(gQueryAnalysisSampleExpirationSecs.load());
                auto doc = SampledQueryDocument{sampledUpdateCmd.sampleId,
                                                sampledUpdateCmd.nss,
                                                *collUuid,
                                                cmdName,
                                                std::move(sampledUpdateCmd.cmd),
                                                expireAt}
                               .toBSON();

                stdx::lock_guard<stdx::mutex> lk(_mutex);
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
        .onError([this, nss = std::move(nss)](Status status) {
            LOGV2(7075301,
                  "Failed to add update query",
                  logAttrs(nss),
                  "error"_attr = redact(status));
        });
}

ExecutorFuture<void> QueryAnalysisWriter::addUpdateQuery(
    OperationContext* originalOpCtx,
    const UUID& sampleId,
    const write_ops::UpdateCommandRequest& updateCmd,
    int opIndex) {
    return addUpdateQuery(
        SampledCommandNameEnum::kUpdate,
        makeSampledUpdateCommandRequest(originalOpCtx, sampleId, updateCmd, opIndex));
}

ExecutorFuture<void> QueryAnalysisWriter::addUpdateQuery(
    OperationContext* opCtx, const write_ops::UpdateCommandRequest& updateCmd, int opIndex) {
    auto sampleId = updateCmd.getUpdates()[opIndex].getSampleId();
    invariant(sampleId);
    return addUpdateQuery(opCtx, *sampleId, updateCmd, opIndex);
}

ExecutorFuture<void> QueryAnalysisWriter::addDeleteQuery(SampledCommandNameEnum cmdName,
                                                         SampledCommandRequest sampledDeleteCmd) {
    invariant(_executor);
    invariant(cmdName == SampledCommandNameEnum::kDelete ||
              cmdName == SampledCommandNameEnum::kBulkWrite);

    auto nss = sampledDeleteCmd.nss;

    return ExecutorFuture<void>(_executor)
        .then(
            [this, cmdName = std::move(cmdName), sampledDeleteCmd = std::move(sampledDeleteCmd)]() {
                auto opCtxHolder = cc().makeOperationContext();
                auto opCtx = opCtxHolder.get();

                auto collUuid = getCollectionUUID(opCtx, sampledDeleteCmd.nss);
                if (!shouldPersistSample(opCtx, sampledDeleteCmd.nss, collUuid)) {
                    return;
                }

                auto expireAt = opCtx->getServiceContext()->getFastClockSource()->now() +
                    mongo::Seconds(gQueryAnalysisSampleExpirationSecs.load());
                auto doc = SampledQueryDocument{sampledDeleteCmd.sampleId,
                                                sampledDeleteCmd.nss,
                                                *collUuid,
                                                cmdName,
                                                std::move(sampledDeleteCmd.cmd),
                                                expireAt}
                               .toBSON();

                stdx::lock_guard<stdx::mutex> lk(_mutex);
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
        .onError([this, nss = std::move(nss)](Status status) {
            LOGV2(7075303,
                  "Failed to add delete query",
                  logAttrs(nss),
                  "error"_attr = redact(status));
        });
}

ExecutorFuture<void> QueryAnalysisWriter::addDeleteQuery(
    OperationContext* originalOpCtx,
    const UUID& sampleId,
    const write_ops::DeleteCommandRequest& deleteCmd,
    int opIndex) {
    return addDeleteQuery(
        SampledCommandNameEnum::kDelete,
        makeSampledDeleteCommandRequest(originalOpCtx, sampleId, deleteCmd, opIndex));
}

ExecutorFuture<void> QueryAnalysisWriter::addDeleteQuery(
    OperationContext* opCtx, const write_ops::DeleteCommandRequest& deleteCmd, int opIndex) {
    auto sampleId = deleteCmd.getDeletes()[opIndex].getSampleId();
    invariant(sampleId);
    return addDeleteQuery(opCtx, *sampleId, deleteCmd, opIndex);
}

ExecutorFuture<void> QueryAnalysisWriter::addFindAndModifyQuery(
    OperationContext* originalOpCtx,
    const UUID& sampleId,
    const write_ops::FindAndModifyCommandRequest& findAndModifyCmd) {
    invariant(_executor);

    return ExecutorFuture<void>(_executor)
        .then([this,
               sampledFindAndModifyCmd = makeSampledFindAndModifyCommandRequest(
                   originalOpCtx, sampleId, findAndModifyCmd)]() mutable {
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();

            auto collUuid = getCollectionUUID(opCtx, sampledFindAndModifyCmd.nss);
            if (!shouldPersistSample(opCtx, sampledFindAndModifyCmd.nss, collUuid)) {
                return;
            }

            auto expireAt = opCtx->getServiceContext()->getFastClockSource()->now() +
                mongo::Seconds(gQueryAnalysisSampleExpirationSecs.load());
            auto doc = SampledQueryDocument{sampledFindAndModifyCmd.sampleId,
                                            sampledFindAndModifyCmd.nss,
                                            *collUuid,
                                            SampledCommandNameEnum::kFindAndModify,
                                            std::move(sampledFindAndModifyCmd.cmd),
                                            expireAt}
                           .toBSON();

            stdx::lock_guard<stdx::mutex> lk(_mutex);
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
    OperationContext* opCtx, const write_ops::FindAndModifyCommandRequest& findAndModifyCmd) {
    auto sampleId = findAndModifyCmd.getSampleId();
    invariant(sampleId);
    return addFindAndModifyQuery(opCtx, *sampleId, findAndModifyCmd);
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

            if (collUuid != getCollectionUUID(opCtx, nss)) {
                return;
            }

            if (!shouldPersistSample(opCtx, nss, collUuid)) {
                return;
            }

            auto expireAt = opCtx->getServiceContext()->getFastClockSource()->now() +
                mongo::Seconds(gQueryAnalysisSampleExpirationSecs.load());
            auto doc =
                SampledQueryDiffDocument{sampleId, nss, collUuid, std::move(*diff), expireAt};

            stdx::lock_guard<stdx::mutex> lk(_mutex);
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
