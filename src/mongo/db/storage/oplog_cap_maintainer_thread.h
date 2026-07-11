// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/storage/oplog_truncate_markers.h"
#include "mongo/util/background.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {
void startOplogCapMaintainerThread(ServiceContext* serviceContext,
                                   bool isReplSet,
                                   bool shouldSkipOplogSampling);

void stopOplogCapMaintainerThread(ServiceContext* serviceContext, const Status& reason);

/**
 * Responsible for deleting oplog truncate markers once their max capacity has been reached.
 */
class [[MONGO_MOD_OPEN]] OplogCapMaintainerThread : public BackgroundJob {
public:
    OplogCapMaintainerThread() : BackgroundJob(false /* deleteSelf */) {}

    static OplogCapMaintainerThread* get(ServiceContext* serviceCtx);
    static OplogCapMaintainerThread* get(OperationContext* opCtx);
    static void set(ServiceContext* serviceCtx,
                    std::unique_ptr<OplogCapMaintainerThread> oplogCapMaintainerThread);

    std::string name() const override {
        return _name;
    }

    void run() override;

    /**
     * Waits until the maintainer thread finishes. Must not be called concurrently with start().
     */
    void shutdown(const Status& reason);

private:
    /**
     * Options for lock acquisition with an intent appropriate to the oplog being truncated.
     */
    virtual Lock::GlobalLockOptions _getOplogTruncationLockOptions();

    /**
     * Returns true iff there was an oplog to delete from.
     */
    bool _deleteExcessDocuments(OperationContext* opCtx);

    /**
     * Pass-through method for reclaiming oplog appropriately to the oplog being truncated.
     * If truncation occurred, the returned RecordId will have isNull() == false.
     */
    virtual RecordId _reclaimOplog(OperationContext* opCtx,
                                   RecordStore& rs,
                                   RecordId mayTruncateUpTo);

    /*
     * Initialize truncation marker creation. This may use scanning or async sampling, depending on
     * what marker creation method is chosen.
     */
    virtual std::shared_ptr<OplogTruncateMarkers> _createInitialMarkers(OperationContext* opCtx,
                                                                        RecordStore& rs) const;


    /*
     * If the cap maintainer thread is responsible for clearing the truncation markers, shut them
     * down and clear them from persistent state. Must be called while holding mutex on _uniqueCtx.
     */
    void _clearTruncationMarkers(const Status& reason);

    // Serializes setting/resetting _uniqueCtx and marking _uniqueCtx killed.
    mutable std::mutex _opCtxMutex;

    // Saves a reference to the cap maintainer thread's operation context.
    boost::optional<ServiceContext::UniqueOperationContext> _uniqueCtx;

    mutable std::mutex _stateMutex;
    bool _shuttingDown = false;
    Status _shutdownReason = Status::OK();

    std::string _name = std::string("OplogCapMaintainerThread-") +
        toStringForLogging(NamespaceString::kRsOplogNamespace);
};

}  // namespace mongo
