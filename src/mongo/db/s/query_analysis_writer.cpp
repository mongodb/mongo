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

#include "mongo/client/connpool.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/server_options.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/analyze_shard_key_server_parameters_gen.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/concurrency/thread_pool.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace analyze_shard_key {

namespace {

MONGO_FAIL_POINT_DEFINE(disableQueryAnalysisWriter);
MONGO_FAIL_POINT_DEFINE(hangQueryAnalysisWriterBeforeWritingLocally);
MONGO_FAIL_POINT_DEFINE(hangQueryAnalysisWriterBeforeWritingRemotely);

const auto getQueryAnalysisWriter = ServiceContext::declareDecoration<QueryAnalysisWriter>();

constexpr int kMaxRetriesOnRetryableErrors = 5;
const WriteConcernOptions kMajorityWriteConcern{WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kWriteConcernTimeoutSystem};

// The size limit for the documents to an insert in a single batch. Leave some padding for other
// fields in the insert command.
constexpr int kMaxBSONObjSizeForInsert = BSONObjMaxUserSize - 500 * 1024;

/*
 * Returns true if this mongod can accept writes to the given collection. Unless the collection is
 * in the "local" database, this will only return true if this mongod is a primary (or a
 * standalone).
 */
bool canAcceptWrites(OperationContext* opCtx, const NamespaceString& ns) {
    ShouldNotConflictWithSecondaryBatchApplicationBlock noPBWMBlock(opCtx->lockState());
    Lock::DBLock lk(opCtx, ns.dbName(), MODE_IS);
    Lock::CollectionLock lock(opCtx, ns, MODE_IS);
    return mongo::repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(opCtx,
                                                                                       ns.db());
}

/*
 * Runs the given write command against the given database locally, asserts that the top-level
 * command is OK, then asserts the write status using the given 'uassertWriteStatusCb' callback.
 * Returns the command response.
 */
BSONObj executeWriteCommandLocal(OperationContext* opCtx,
                                 const std::string dbName,
                                 const BSONObj& cmdObj,
                                 const std::function<void(const BSONObj&)>& uassertWriteStatusCb) {
    DBDirectClient client(opCtx);
    BSONObj resObj;

    if (!client.runCommand(dbName, cmdObj, resObj)) {
        uassertStatusOK(getStatusFromCommandResult(resObj));
    }
    uassertWriteStatusCb(resObj);

    return resObj;
}

/*
 * Runs the given write command against the given database on the (remote) primary, asserts that the
 * top-level command is OK, then asserts the write status using the given 'uassertWriteStatusCb'
 * callback. Throws a PrimarySteppedDown error if no primary is found. Returns the command response.
 */
BSONObj executeWriteCommandRemote(OperationContext* opCtx,
                                  const std::string dbName,
                                  const BSONObj& cmdObj,
                                  const std::function<void(const BSONObj&)>& uassertWriteStatusCb) {
    auto hostAndPort = repl::ReplicationCoordinator::get(opCtx)->getCurrentPrimaryHostAndPort();

    if (hostAndPort.empty()) {
        uasserted(ErrorCodes::PrimarySteppedDown, "No primary exists currently");
    }

    auto conn = std::make_unique<ScopedDbConnection>(hostAndPort.toString());

    if (auth::isInternalAuthSet()) {
        uassertStatusOK(conn->get()->authenticateInternalUser());
    }

    DBClientBase* client = conn->get();
    ScopeGuard guard([&] { conn->done(); });
    try {
        BSONObj resObj;

        if (!client->runCommand(dbName, cmdObj, resObj)) {
            uassertStatusOK(getStatusFromCommandResult(resObj));
        }
        uassertWriteStatusCb(resObj);

        return resObj;
    } catch (...) {
        guard.dismiss();
        conn->kill();
        throw;
    }
}

/*
 * Runs the given write command against the given collection. If this mongod is currently the
 * primary, runs the write command locally. Otherwise, runs the command on the remote primary.
 * Internally asserts that the top-level command is OK, then asserts the write status using the
 * given 'uassertWriteStatusCb' callback. Internally retries the write command on retryable errors
 * (for kMaxRetriesOnRetryableErrors times) so the writes must be idempotent. Returns the
 * command response.
 */
BSONObj executeWriteCommand(OperationContext* opCtx,
                            const NamespaceString& ns,
                            const BSONObj& cmdObj,
                            const std::function<void(const BSONObj&)>& uassertWriteStatusCb) {
    const auto dbName = ns.db().toString();
    auto numRetries = 0;

    while (true) {
        try {
            if (canAcceptWrites(opCtx, ns)) {
                // There is a window here where this mongod may step down after check above. In this
                // case, a NotWritablePrimary error would be thrown. However, this is preferable to
                // running the command while holding locks.
                hangQueryAnalysisWriterBeforeWritingLocally.pauseWhileSet(opCtx);
                return executeWriteCommandLocal(opCtx, dbName, cmdObj, uassertWriteStatusCb);
            }

            hangQueryAnalysisWriterBeforeWritingRemotely.pauseWhileSet(opCtx);
            return executeWriteCommandRemote(opCtx, dbName, cmdObj, uassertWriteStatusCb);
        } catch (DBException& ex) {
            if (ErrorCodes::isRetriableError(ex) && numRetries < kMaxRetriesOnRetryableErrors) {
                numRetries++;
                continue;
            }
            throw;
        }
    }

    return {};
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
            if (doc.objsize() + objSize >= kMaxBSONObjSizeForInsert) {
                break;
            }
            lastIndex--;
            objSize += doc.objsize();
            docsToInsert.push_back(std::move(doc));
        }
        // We don't add a document that is above the size limit to the buffer so we should have
        // added at least one document to 'docsToInsert'.
        invariant(!docsToInsert.empty());

        write_ops::InsertCommandRequest insertCmd(ns);
        insertCmd.setDocuments(std::move(docsToInsert));
        insertCmd.setWriteCommandRequestBase([&] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(false);
            wcb.setBypassDocumentValidation(false);
            return wcb;
        }());
        auto insertCmdBson = insertCmd.toBSON(
            {BSON(WriteConcernOptions::kWriteConcernField << kMajorityWriteConcern.toBSON())});

        executeWriteCommand(opCtx, ns, std::move(insertCmdBson), [&](const BSONObj& resObj) {
            BatchedCommandResponse response;
            std::string errMsg;

            if (!response.parseBSON(resObj, &errMsg)) {
                uasserted(ErrorCodes::FailedToParse, errMsg);
            }

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
    if (doc.objsize() > kMaxBSONObjSizeForInsert) {
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
    return _queries.getSize() >= gQueryAnalysisWriterMaxMemoryUsageBytes.load();
}

ExecutorFuture<void> QueryAnalysisWriter::addFindQuery(const UUID& sampleId,
                                                       const NamespaceString& nss,
                                                       const BSONObj& filter,
                                                       const BSONObj& collation) {
    return _addReadQuery(sampleId, nss, SampledReadCommandNameEnum::kFind, filter, collation);
}

ExecutorFuture<void> QueryAnalysisWriter::addCountQuery(const UUID& sampleId,
                                                        const NamespaceString& nss,
                                                        const BSONObj& filter,
                                                        const BSONObj& collation) {
    return _addReadQuery(sampleId, nss, SampledReadCommandNameEnum::kCount, filter, collation);
}

ExecutorFuture<void> QueryAnalysisWriter::addDistinctQuery(const UUID& sampleId,
                                                           const NamespaceString& nss,
                                                           const BSONObj& filter,
                                                           const BSONObj& collation) {
    return _addReadQuery(sampleId, nss, SampledReadCommandNameEnum::kDistinct, filter, collation);
}

ExecutorFuture<void> QueryAnalysisWriter::addAggregateQuery(const UUID& sampleId,
                                                            const NamespaceString& nss,
                                                            const BSONObj& filter,
                                                            const BSONObj& collation) {
    return _addReadQuery(sampleId, nss, SampledReadCommandNameEnum::kAggregate, filter, collation);
}

ExecutorFuture<void> QueryAnalysisWriter::_addReadQuery(const UUID& sampleId,
                                                        const NamespaceString& nss,
                                                        SampledReadCommandNameEnum cmdName,
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
            auto doc = SampledReadQueryDocument{sampleId, nss, *collUuid, cmdName, cmd.toBSON()};
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

}  // namespace analyze_shard_key
}  // namespace mongo
