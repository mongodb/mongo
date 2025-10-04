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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/mutable_observer_registry.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/analyze_shard_key_role.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/future.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace analyze_shard_key {

struct SampledCommandRequest {
    UUID sampleId;
    NamespaceString nss;
    // The BSON for a SampledReadCommand or {Update,Delete,FindAndModify}CommandRequest.
    BSONObj cmd;
};

/**
 * Owns the machinery for persisting sampled queries. That consists of the following:
 * - The buffer that stores sampled queries and the periodic background job that inserts those
 *   queries into the local config.sampledQueries collection.
 * - The buffer that stores diffs for sampled update queries and the periodic background job that
 *   inserts those diffs into the local config.sampledQueriesDiff collection.
 *
 * On a sharded cluster, a writer is any shardsvr mongod in the cluster. On a standalone replica
 * set, a writer is any mongod in the set. If the mongod is a primary, it will execute the
 * insert commands locally. If it is a secondary, it will perform the insert commands against the
 * primary.
 *
 * The memory usage of the buffers is controlled by the 'queryAnalysisWriterMaxMemoryUsageBytes'
 * server parameter. Upon adding a query or a diff that causes the total size of buffers to exceed
 * the limit, the writer will flush the corresponding buffer immediately instead of waiting for it
 * to get flushed later by the periodic job.
 */
class QueryAnalysisWriter final : public std::enable_shared_from_this<QueryAnalysisWriter>,
                                  public ReplicaSetAwareService<QueryAnalysisWriter> {
    QueryAnalysisWriter(const QueryAnalysisWriter&) = delete;
    QueryAnalysisWriter& operator=(const QueryAnalysisWriter&) = delete;

public:
    static const std::string kSampledQueriesTTLIndexName;
    static BSONObj kSampledQueriesTTLIndexSpec;

    static const std::string kSampledQueriesDiffTTLIndexName;
    static BSONObj kSampledQueriesDiffTTLIndexSpec;

    static const std::string kAnalyzeShardKeySplitPointsTTLIndexName;
    static BSONObj kAnalyzeShardKeySplitPointsTTLIndexSpec;

    static const std::map<NamespaceString, BSONObj> kTTLIndexes;

    static const std::set<ErrorCodes::Error> kNonRetryableInsertErrorCodes;

    /**
     * Temporarily stores documents to be written to disk.
     */
    struct Buffer {
    public:
        Buffer(const NamespaceString& nss) : _nss(nss) {};

        const NamespaceString& getNss() const {
            return _nss;
        }

        /**
         * Adds the given document to the buffer if its size is below the limit (i.e.
         * BSONObjMaxUserSize - some padding) and increments the total number of bytes accordingly.
         * Returns true unless the document's size exceeds the limit.
         */
        bool add(BSONObj doc);

        /**
         * Removes the documents at 'index' onwards from the buffer and decrements the total number
         * of the bytes by 'numBytes'. The caller must ensure that that 'numBytes' is indeed the
         * total size of the documents being removed.
         */
        void truncate(size_t index, long long numBytes);

        bool isEmpty() const {
            return _docs.empty();
        }

        int getCount() const {
            return _docs.size();
        }

        long long getSize() const {
            return _numBytes;
        }

        BSONObj at(size_t index) const {
            return _docs[index];
        }

        std::vector<BSONObj> getDocuments() const {
            return _docs;
        }

    private:
        NamespaceString _nss;

        std::vector<BSONObj> _docs;
        long long _numBytes = 0;
    };

    QueryAnalysisWriter() = default;
    ~QueryAnalysisWriter() override = default;

    QueryAnalysisWriter(QueryAnalysisWriter&& source) = delete;
    QueryAnalysisWriter& operator=(QueryAnalysisWriter&& other) = delete;

    /**
     * Obtains the service-wide QueryAnalysisWriter instance.
     */
    static QueryAnalysisWriter* get(OperationContext* opCtx);
    static QueryAnalysisWriter* get(ServiceContext* serviceContext);

    static inline MutableObserverRegistry<int32_t> observeQueryAnalysisWriterIntervalSecs;

    /**
     * ReplicaSetAwareService methods:
     */
    void onStartup(OperationContext* opCtx) override;
    void onShutdown() override;
    void onStepUpComplete(OperationContext* opCtx, long long term) override;
    inline std::string getServiceName() const final {
        return "QueryAnalysisWriter";
    }

    ExecutorFuture<void> createTTLIndexes(OperationContext* opCtx);

    ExecutorFuture<void> addFindQuery(const UUID& sampleId,
                                      const NamespaceString& nss,
                                      const BSONObj& filter,
                                      const BSONObj& collation,
                                      const boost::optional<BSONObj>& letParameters);

    ExecutorFuture<void> addAggregateQuery(const UUID& sampleId,
                                           const NamespaceString& nss,
                                           const BSONObj& filter,
                                           const BSONObj& collation,
                                           const boost::optional<BSONObj>& letParameters);

    ExecutorFuture<void> addCountQuery(const UUID& sampleId,
                                       const NamespaceString& nss,
                                       const BSONObj& filter,
                                       const BSONObj& collation);

    ExecutorFuture<void> addDistinctQuery(const UUID& sampleId,
                                          const NamespaceString& nss,
                                          const BSONObj& filter,
                                          const BSONObj& collation);

    ExecutorFuture<void> addUpdateQuery(SampledCommandNameEnum cmdName,
                                        SampledCommandRequest sampledUpdateCmd);
    ExecutorFuture<void> addUpdateQuery(OperationContext* opCtx,
                                        const UUID& sampleId,
                                        const write_ops::UpdateCommandRequest& updateCmd,
                                        int opIndex);
    ExecutorFuture<void> addUpdateQuery(OperationContext* opCtx,
                                        const write_ops::UpdateCommandRequest& updateCmd,
                                        int opIndex);

    ExecutorFuture<void> addDeleteQuery(SampledCommandNameEnum cmdName,
                                        SampledCommandRequest sampledDeleteCmd);
    ExecutorFuture<void> addDeleteQuery(OperationContext* opCtx,
                                        const UUID& sampleId,
                                        const write_ops::DeleteCommandRequest& deleteCmd,
                                        int opIndex);
    ExecutorFuture<void> addDeleteQuery(OperationContext* opCtx,
                                        const write_ops::DeleteCommandRequest& deleteCmd,
                                        int opIndex);

    ExecutorFuture<void> addFindAndModifyQuery(
        OperationContext* opCtx,
        const UUID& sampleId,
        const write_ops::FindAndModifyCommandRequest& findAndModifyCmd);
    ExecutorFuture<void> addFindAndModifyQuery(
        OperationContext* opCtx, const write_ops::FindAndModifyCommandRequest& findAndModifyCmd);

    ExecutorFuture<void> addDiff(const UUID& sampleId,
                                 const NamespaceString& nss,
                                 const UUID& collUuid,
                                 const BSONObj& preImage,
                                 const BSONObj& postImage);

    int getQueriesCountForTest() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _queries.getCount();
    }

    std::vector<BSONObj> getQueriesForTest() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _queries.getDocuments();
    }

    void flushQueriesForTest(OperationContext* opCtx) {
        _flushQueries(opCtx);
    }

    int getDiffsCountForTest() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _diffs.getCount();
    }

    std::vector<BSONObj> getDiffsForTest() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _diffs.getDocuments();
    }

    void flushDiffsForTest(OperationContext* opCtx) {
        _flushDiffs(opCtx);
    }

private:
    bool shouldRegisterReplicaSetAwareService() const final;

    void onConsistentDataAvailable(OperationContext* opCtx,
                                   bool isMajority,
                                   bool isRollback) final {}

    void onStepUpBegin(OperationContext* opCtx, long long term) final {}

    void onStepDown() final {
        _isPrimary.store(false);
    }

    void onRollbackBegin() final {
        _isPrimary.store(false);
    }

    void onBecomeArbiter() final {}

    void onSetCurrentConfig(OperationContext* opCtx) final {}

    ExecutorFuture<void> _addReadQuery(const UUID& sampleId,
                                       const NamespaceString& nss,
                                       SampledCommandNameEnum cmdName,
                                       const BSONObj& filter,
                                       const BSONObj& collation,
                                       const boost::optional<BSONObj>& letParameters = boost::none);

    void _flushQueries(OperationContext* opCtx);
    void _flushDiffs(OperationContext* opCtx);

    /**
     * The helper for '_flushQueries' and '_flushDiffs'. Inserts the documents in 'buffer' into the
     * collection it is associated with in batches, and removes all the inserted documents from
     * 'buffer'. Internally retries the inserts on retryable errors for a fixed number of times.
     * Ignores DuplicateKey errors since they are expected for the following reasons:
     * - For the query buffer, a sampled query that is idempotent (e.g. a read or retryable write)
     *   could get added to the buffer (across nodes) more than once due to retries.
     * - For the diff buffer, a sampled multi-update query could end up generating multiple diffs
     *   and each diff is identified using the sample id of the sampled query that creates it.
     *
     * Throws an error if the inserts fail with any other error.
     */
    void _flush(OperationContext* opCtx, stdx::unique_lock<stdx::mutex>& lk, Buffer* buffer);

    /**
     * Returns true if the total size of the buffered queries and diffs has exceeded the maximum
     * amount of memory that the writer is allowed to use.
     */
    bool _exceedsMaxSizeBytes();

    AtomicWord<bool> _isPrimary{false};

    mutable stdx::mutex _mutex;

    std::shared_ptr<PeriodicJobAnchor> _periodicQueryWriter;
    Buffer _queries{NamespaceString::kConfigSampledQueriesNamespace};

    std::shared_ptr<PeriodicJobAnchor> _periodicDiffWriter;
    Buffer _diffs{NamespaceString::kConfigSampledQueriesDiffNamespace};

    // Initialized on startup and joined on shutdown.
    std::shared_ptr<executor::TaskExecutor> _executor;
};

}  // namespace analyze_shard_key
}  // namespace mongo
