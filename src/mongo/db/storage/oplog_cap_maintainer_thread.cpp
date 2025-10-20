/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include <exception>
#include <mutex>
#include <utility>

#include "oplog_cap_maintainer_thread.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/wiredtiger/oplog_truncate_marker_parameters_gen.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/decorable.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

const auto getMaintainerThread =
    ServiceContext::declareDecoration<std::unique_ptr<OplogCapMaintainerThread>>();

// Cumulative number of times the thread has been interrupted
AtomicWord<int64_t> interruptCount;

MONGO_FAIL_POINT_DEFINE(hangOplogCapMaintainerThread);
MONGO_FAIL_POINT_DEFINE(hangBeforeOplogSampling);

class OplogTruncateMarkersServerStatusSection : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    /**
     * <ServerStatusSection>
     */
    bool includeByDefault() const override {
        return true;
    }

    /**
     * <ServerStatusSection>
     */
    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder builder;
        builder.append("interruptCount", interruptCount.load());
        return builder.obj();
    }
};

auto oplogTruncateMarkersStats =
    *ServerStatusSectionBuilder<OplogTruncateMarkersServerStatusSection>("oplogTruncationThread")
         .forShard();

}  // namespace

void startOplogCapMaintainerThread(ServiceContext* serviceContext,
                                   bool isReplSet,
                                   bool shouldSkipOplogSampling) {
    if (shouldSkipOplogSampling) {
        return;
    }

    if (!isReplSet) {
        return;
    }

    if (storageGlobalParams.queryableBackupMode || storageGlobalParams.repair) {
        return;
    }

    if (!serviceContext->userWritesAllowed()) {
        return;
    }

    std::unique_ptr<OplogCapMaintainerThread> maintainerThread =
        std::make_unique<OplogCapMaintainerThread>();
    OplogCapMaintainerThread::set(serviceContext, std::move(maintainerThread));
    OplogCapMaintainerThread::get(serviceContext)->go();
}

void stopOplogCapMaintainerThread(ServiceContext* serviceContext, const Status& reason) {
    if (OplogCapMaintainerThread* maintainerThread = OplogCapMaintainerThread::get(serviceContext);
        maintainerThread) {
        maintainerThread->shutdown(reason);
    }
}

OplogCapMaintainerThread* OplogCapMaintainerThread::get(ServiceContext* serviceCtx) {
    auto& maintainerThread = getMaintainerThread(serviceCtx);
    if (maintainerThread) {
        return maintainerThread.get();
    }

    return nullptr;
}

OplogCapMaintainerThread* OplogCapMaintainerThread::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void OplogCapMaintainerThread::set(
    ServiceContext* serviceCtx,

    std::unique_ptr<OplogCapMaintainerThread> oplogCapMaintainerThread) {
    auto& maintainerThread = getMaintainerThread(serviceCtx);
    if (maintainerThread) {
        invariant(!maintainerThread->running(),
                  "Tried to reset the OplogCapMaintainerThread without shutting down the original "
                  "instance.");
    }

    invariant(oplogCapMaintainerThread);
    maintainerThread = std::move(oplogCapMaintainerThread);
}

bool OplogCapMaintainerThread::_deleteExcessDocuments(OperationContext* opCtx) {
    if (!opCtx->getServiceContext()->getStorageEngine()) {
        LOGV2_DEBUG(22240, 2, "OplogCapMaintainerThread: no global storage engine yet");
        return false;
    }

    // Maintaining the Oplog cap is crucial to the stability of the server so that we don't let the
    // oplog grow unbounded. We mark the operation as having immediate priority to skip ticket
    // acquisition and flow control.
    ScopedAdmissionPriority<ExecutionAdmissionContext> priority(
        opCtx, AdmissionContext::Priority::kExempt);

    try {
        // A Global IX lock should be good enough to protect the oplog truncation from
        // interruptions such as replication rollback. Database lock or collection lock is not
        // needed. This improves concurrency if oplog truncation takes long time.
        std::shared_ptr<CollectionTruncateMarkers> oplogTruncateMarkers;
        {
            Lock::GlobalLock globalLk(opCtx, MODE_IX);
            auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();
            if (!rs) {
                LOGV2_DEBUG(4562600, 2, "oplog collection does not exist");
                return false;
            }
            LOGV2(10621107,
                  "Deleting excess documents",
                  "Oplog size (in bytes)"_attr = rs->dataSize(opCtx));

            // Create another reference to the oplog truncate markers while holding a lock on
            // the collection to prevent it from being destructed.
            oplogTruncateMarkers = rs->getCollectionTruncateMarkers();
            invariant(oplogTruncateMarkers);
        }

        if (!oplogTruncateMarkers->awaitHasExcessMarkersOrDead(opCtx)) {
            // Oplog went away or we timed out waiting for oplog space to reclaim.
            return false;
        }

        {
            // Oplog state could have changed while yielding. Reacquire global lock
            // and refresh oplog state to ensure we have a valid pointer.
            Lock::GlobalLock globalLk(opCtx, MODE_IX);
            auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();
            if (!rs) {
                LOGV2_DEBUG(9064300, 2, "oplog collection does not exist");
                return false;
            }
            rs->reclaimOplog(opCtx);
        }
    } catch (ExceptionForCat<ErrorCategory::Interruption>& ex) {
        LOGV2(11212204, "OplogCapMaintainerThread interrupted", "reason"_attr = ex.reason());
        interruptCount.fetchAndAdd(1);
    } catch (const std::exception& e) {
        LOGV2_FATAL_NOTRACE(22243, "Error in OplogCapMaintainerThread", "error"_attr = e.what());
    } catch (...) {
        LOGV2_FATAL_NOTRACE(5184100, "Unknown error in OplogCapMaintainerThread");
    }
    return true;
}

void OplogCapMaintainerThread::run() {
    LOGV2(5295000, "Oplog cap maintainer thread started", "threadName"_attr = _name);
    ThreadClient tc(_name, getGlobalServiceContext()->getService(ClusterRole::ShardServer));

    {
        stdx::lock_guard<Client> lk(*tc.get());
        tc.get()->setSystemOperationUnkillableByStepdown(lk);
    }

    boost::optional<ScopedAdmissionPriority<ExecutionAdmissionContext>> admissionPriority;

    {
        stdx::lock_guard<stdx::mutex> lk(_opCtxMutex);

        // Initialize the thread's opCtx.
        _uniqueCtx.emplace(tc->makeOperationContext());

        // Maintaining the Oplog cap is crucial to the stability of the server so that we don't
        // let the oplog grow unbounded. We mark the operation as having immediate priority to
        // skip ticket acquisition and flow control.
        admissionPriority.emplace(_uniqueCtx->get(), AdmissionContext::Priority::kExempt);
    }

    ON_BLOCK_EXIT([&] {
        stdx::lock_guard<stdx::mutex> lk(_opCtxMutex);
        admissionPriority.reset();
        _uniqueCtx.reset();
    });


    if (gOplogSamplingAsyncEnabled) {
        try {
            {
                stdx::unique_lock<stdx::mutex> lk(_stateMutex);
                if (_shuttingDown) {
                    return;
                }
            }

            if (MONGO_unlikely(hangBeforeOplogSampling.shouldFail())) {
                LOGV2(11212200, "Hanging due to 'hangBeforeSampling' fail point");
                hangBeforeOplogSampling.pauseWhileSet(_uniqueCtx->get());
            }

            // Need the oplog to have been created first before we proceed.
            do {
                // Create the initial set of truncate markers as part of this thread before we
                // attempt to delete excess markers. Ensure that the oplog has been created as part
                // of restart before we attempt to create markers.
                boost::optional<AutoGetOplogFastPath> oplogRead;
                oplogRead.emplace(_uniqueCtx->get(),
                                  OplogAccessMode::kRead,
                                  Date_t::max(),
                                  AutoGetOplogFastPathOptions{.skipRSTLLock = true});

                const auto& oplog = oplogRead->getCollection();
                if (oplog) {
                    oplog->getRecordStore()->sampleAndUpdate(_uniqueCtx->get());
                    break;
                }

                // Wait a bit to give the oplog a chance to be created.
                MONGO_IDLE_THREAD_BLOCK;
                LOGV2_DEBUG(10621101, 1, "OplogCapMaintainerThread is idle");

                // Reset the oplogRead so we don't hold a lock while we sleep.
                oplogRead.reset();
                sleepFor(Milliseconds(100));
                LOGV2_DEBUG(10621109, 1, "OplogCapMaintainerThread is active");
            } while (true);
        } catch (ExceptionForCat<ErrorCategory::Interruption>& ex) {
            LOGV2(11212201, "OplogCapMaintainerThread interrupted", "reason"_attr = ex.reason());
            interruptCount.fetchAndAdd(1);
            return;
        }
    }

    while (true) {
        // We need this check since the first check to _shuttingDown is guarded by
        // gOplogSamplingAsyncEnabled and we will never check this value if async is disabled.
        {
            stdx::unique_lock<stdx::mutex> lk(_stateMutex);
            if (_shuttingDown) {
                return;
            }
        }

        if (MONGO_unlikely(hangOplogCapMaintainerThread.shouldFail())) {
            LOGV2(5095500, "Hanging the oplog cap maintainer thread due to fail point");
            try {
                hangOplogCapMaintainerThread.pauseWhileSet(_uniqueCtx->get());
            } catch (DBException& ex) {
                auto interruptStatus = _uniqueCtx->get()->checkForInterruptNoAssert();
                if (!interruptStatus.isOK()) {
                    LOGV2(9064302,
                          "Stop hanging the oplog cap maintainer thread due to interrupted fail "
                          "point wait",
                          "ex"_attr = ex,
                          "interruptStatus"_attr = interruptStatus,
                          "shutdown"_attr = globalInShutdownDeprecated());
                    continue;
                }
                throw;
            }
        }

        if (!_deleteExcessDocuments(_uniqueCtx->get()) && !globalInShutdownDeprecated()) {
            sleepmillis(1000);  // Back off in case there were problems deleting.
        }
    }
}


void OplogCapMaintainerThread::shutdown(const Status& reason) {
    LOGV2_INFO(7474902, "Shutting down oplog cap maintainer thread", "reason"_attr = reason);

    {
        stdx::lock_guard<stdx::mutex> lk(_opCtxMutex);
        if (_uniqueCtx) {
            stdx::lock_guard<Client> lk(*_uniqueCtx->get()->getClient());
            _uniqueCtx->get()->markKilled(reason.code());
        }
    }

    {
        stdx::lock_guard<stdx::mutex> lk(_stateMutex);
        _shuttingDown = true;
        _shutdownReason = reason;
    }

    wait();
    LOGV2(7474901, "Finished shutting down oplog cap maintainer thread");
}

}  // namespace mongo
