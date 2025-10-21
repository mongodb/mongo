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

#include "oplog_cap_maintainer_thread.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/local_oplog_info.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/intent_registry.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/oplog_truncate_marker_parameters_gen.h"
#include "mongo/db/storage/oplog_truncation.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

const auto getMaintainerThread =
    ServiceContext::declareDecoration<std::unique_ptr<OplogCapMaintainerThread>>();

// Cumulative amount of time spent truncating the oplog.
AtomicWord<int64_t> totalTimeTruncating;

// Cumulative number of truncates of the oplog.
AtomicWord<int64_t> truncateCount;

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

        // Hold reference to the catalog for collection lookup without locks to be safe.
        auto catalog = CollectionCatalog::get(opCtx);
        auto oplogCollection =
            catalog->lookupCollectionByNamespace(opCtx, NamespaceString::kRsOplogNamespace);
        if (oplogCollection) {
            // In certain modes, like read-only, no truncate markers are created.
            if (auto truncateMarkers = LocalOplogInfo::get(opCtx)->getTruncateMarkers()) {
                auto method = truncateMarkers->getMarkersCreationMethod();
                if (method == CollectionTruncateMarkers::MarkersCreationMethod::Sampling) {
                    builder.append("totalTimeProcessingMicros",
                                   truncateMarkers->getCreationProcessingTime().count());
                    builder.append("processingMethod", "sampling");
                } else if (method == CollectionTruncateMarkers::MarkersCreationMethod::InProgress) {
                    invariant(truncateMarkers->getCreationProcessingTime().count() == 0);
                    builder.append("totalTimeProcessingMicros", -1);
                    builder.append("processingMethod", "in progress");
                } else if (method ==
                           CollectionTruncateMarkers::MarkersCreationMethod::EmptyCollection) {
                    builder.append("totalTimeProcessingMicros", -1);
                    builder.append("processingMethod", "empty collection");
                } else {
                    builder.append("totalTimeProcessingMicros",
                                   truncateMarkers->getCreationProcessingTime().count());
                    builder.append("processingMethod", "scanning");
                }
                builder.appendNumber("truncateMarkersCount",
                                     static_cast<long long>(truncateMarkers->numMarkers()));
            }
        }

        if (auto oplogMinRetentionHours = storageGlobalParams.oplogMinRetentionHours.load()) {
            builder.append("oplogMinRetentionHours", oplogMinRetentionHours);
        }

        builder.append("totalTimeTruncatingMicros", totalTimeTruncating.load());
        builder.append("truncateCount", truncateCount.load());
        builder.append("interruptCount", interruptCount.load());

        return builder.obj();
    }
};

auto oplogTruncateMarkersStats =
    *ServerStatusSectionBuilder<OplogTruncateMarkersServerStatusSection>("oplogTruncation")
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

    if (!rss::ReplicatedStorageService::get(serviceContext)
             .getPersistenceProvider()
             .supportsOplogSampling()) {
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
    // A Global IX lock should be good enough to protect the oplog truncation from
    // interruptions such as replication rollback. Database lock or collection lock is not
    // needed. This improves concurrency if oplog truncation takes long time.
    std::shared_ptr<CollectionTruncateMarkers> oplogTruncateMarkers;
    {
        Lock::GlobalLock globalLk(
            opCtx,
            MODE_IX,
            {false, false, false, rss::consensus::IntentRegistry::Intent::LocalWrite});
        auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();
        if (!rs) {
            LOGV2_DEBUG(4562600, 2, "oplog collection does not exist");
            return false;
        }
        LOGV2(10621107, "Deleting excess documents", "Oplog size (in bytes)"_attr = rs->dataSize());

        // Create another reference to the oplog truncate markers while holding a lock on
        // the collection to prevent it from being destructed.
        oplogTruncateMarkers = LocalOplogInfo::get(opCtx)->getTruncateMarkers();
        invariant(oplogTruncateMarkers);
    }

    if (!oplogTruncateMarkers->awaitHasExcessMarkersOrDead(opCtx)) {
        // Oplog went away or we timed out waiting for oplog space to reclaim.
        return false;
    }

    {
        // Oplog state could have changed while yielding. Reacquire global lock
        // and refresh oplog state to ensure we have a valid pointer.
        Lock::GlobalLock globalLk(opCtx,
                                  MODE_IX,
                                  Date_t::max(),
                                  Lock::InterruptBehavior::kThrow,
                                  {false,
                                   true /* skipRstl */,
                                   false,
                                   rss::consensus::IntentRegistry::Intent::LocalWrite});
        auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();
        if (!rs) {
            LOGV2_DEBUG(9064300, 2, "oplog collection does not exist");
            return false;
        }

        auto mayTruncateUpTo = opCtx->getServiceContext()->getStorageEngine()->getPinnedOplog();

        Timer timer;
        oplog_truncation::reclaimOplog(opCtx, *rs, RecordId(mayTruncateUpTo.asULL()));

        auto elapsedMicros = timer.micros();
        totalTimeTruncating.fetchAndAdd(elapsedMicros);
        truncateCount.fetchAndAdd(1);

        auto elapsedMillis = elapsedMicros / 1000;
        LOGV2(22402,
              "Oplog truncation finished",
              "pinnedOplogTimestamp"_attr = mayTruncateUpTo,
              "numRecords"_attr = rs->numRecords(),
              "dataSize"_attr = rs->dataSize(),
              "duration"_attr = Milliseconds(elapsedMillis));
    }

    return true;
}

void OplogCapMaintainerThread::run() {
    LOGV2(5295000, "Oplog cap maintainer thread started", "threadName"_attr = _name);
    ThreadClient tc(_name,
                    getGlobalServiceContext()->getService(ClusterRole::ShardServer),
                    Client::noSession(),
                    ClientOperationKillableByStepdown{false});

    boost::optional<ScopedAdmissionPriority<ExecutionAdmissionContext>> admissionPriority;

    {
        stdx::lock_guard<stdx::mutex> lk(_opCtxMutex);

        // Initialize the thread's opCtx.
        _uniqueCtx.emplace(tc->makeOperationContext());

        // Maintaining the Oplog cap is crucial to the stability of the server so that we don't let
        // the oplog grow unbounded. We mark the operation as having immediate priority to skip
        // ticket acquisition and flow control.
        admissionPriority.emplace(_uniqueCtx->get(), AdmissionContext::Priority::kExempt);
    }

    ON_BLOCK_EXIT([&] {
        auto ts = _uniqueCtx->get()->getServiceContext()->getTickSource();
        auto killTime = _uniqueCtx->get()->getKillTime();
        {
            stdx::lock_guard<stdx::mutex> lk(_stateMutex);
            LOGV2(11211800,
                  "Time spent between an operation interrupting the cap maintainer thread and the "
                  "thread successfully shutting down.",
                  "reason"_attr = _shutdownReason,
                  "durationMs"_attr = (ts->ticksTo<Milliseconds>(ts->getTicks() - killTime)));
        }

        {
            stdx::lock_guard<stdx::mutex> lk(_opCtxMutex);
            admissionPriority.reset();
            _uniqueCtx.reset();
        }
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
                oplogRead.emplace(
                    _uniqueCtx->get(),
                    OplogAccessMode::kRead,
                    Date_t::max(),
                    AutoGetOplogFastPathOptions{.skipRSTLLock = true,
                                                .explicitIntent =
                                                    rss::consensus::IntentRegistry::Intent::Read});
                const auto& oplog = oplogRead->getCollection();
                if (oplog) {
                    // Initial sampling and marker creation.
                    auto oplogTruncateMarkers = OplogTruncateMarkers::sampleAndUpdate(
                        _uniqueCtx->get(), *oplog->getRecordStore());
                    invariant(oplogTruncateMarkers);
                    LocalOplogInfo::get(_uniqueCtx->get())
                        ->setTruncateMarkers(std::move(oplogTruncateMarkers));
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
        } catch (ExceptionFor<ErrorCategory::Interruption>& ex) {
            LOGV2(11212201, "OplogCapMaintainerThread interrupted", "status"_attr = ex.toStatus());
            interruptCount.fetchAndAdd(1);

            stdx::lock_guard<stdx::mutex> lk(_stateMutex);
            _shutdownReason = ex.toStatus();

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

        try {
            if (MONGO_unlikely(hangOplogCapMaintainerThread.shouldFail())) {
                LOGV2(5095500, "Hanging the oplog cap maintainer thread due to fail point");
                hangOplogCapMaintainerThread.pauseWhileSet(_uniqueCtx->get());
            }

            if (_deleteExcessDocuments(_uniqueCtx->get())) {
                continue;
            }

            _uniqueCtx->get()->sleepFor(
                Seconds(1));  // Back off in case there were problems deleting.
        } catch (ExceptionFor<ErrorCategory::Interruption>& ex) {
            LOGV2(11212204, "OplogCapMaintainerThread interrupted", "status"_attr = ex.toStatus());
            interruptCount.fetchAndAdd(1);

            stdx::lock_guard<stdx::mutex> lk(_stateMutex);
            _shutdownReason = ex.toStatus();

            return;
        } catch (...) {
            const auto& err = mongo::exceptionToStatus();
            if (_uniqueCtx->get()->checkForInterruptNoAssert().isOK()) {
                LOGV2_FATAL_NOTRACE(
                    6761100, "Error in OplogCapMaintainerThread", "error"_attr = err);
            }
            // Since we make this operation unkillable by stepdown, the opCtx can't be
            // interrupted by repl state transitions - stepdown, stepup, and rollback. It can
            // only be interrupted by shutdown, killOp, or storage change (causes
            // ErrorCodes::InterruptedDueToStorageChange) due to FCBIS. The shutdown case is
            // handled above. We reach here for the last two cases, and it's safe to continue.
            LOGV2_DEBUG(9064301,
                        1,
                        "Oplog cap maintainer thread was interrupted, but can safely continue",
                        "error"_attr = err);
        }
    }

    MONGO_UNREACHABLE;
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
