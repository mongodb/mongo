/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/ftdc/collector.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/client.h"
#include "mongo/db/client_strand.h"
#include "mongo/db/ftdc/constants.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/replica_set_endpoint_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/ctype.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <array>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kFTDC

namespace mongo {

namespace {

auto getCurrentDate(OperationContext* opCtx) {
    return opCtx->getServiceContext()->getPreciseClockSource()->now();
}

}  // namespace

std::tuple<BSONObj, Date_t> FTDCCollectorCollection::collect(Client* client) {
    BSONObjBuilder builder;
    // If there are no collectors, just return an empty BSONObj so that that are caller knows we did
    // not collect anything
    if (empty()) {
        return std::tuple<BSONObj, Date_t>(BSONObj(), Date_t());
    }

    // All collectors should be ok seeing the inconsistent states in the middle of replication
    // batches. This is desirable because we want to be able to collect data in the middle of
    // batches that are taking a long time.
    auto opCtx = client->makeOperationContext();
    opCtx->setEnforceConstraints(false);

    Date_t start = getCurrentDate(opCtx.get());
    builder.appendDate(kFTDCCollectStartField, start);

    ScopedAdmissionPriority<ExecutionAdmissionContext> admissionPriority(
        opCtx.get(), AdmissionContext::Priority::kExempt);

    // Set a secondaryOk=true read preference because some collections run commands such as
    // aggregation which are AllowedOnSecondary::kOptIn.
    ReadPreferenceSetting::get(opCtx.get()) = ReadPreferenceSetting{ReadPreference::Nearest};

    _collect(opCtx.get(), &builder);

    builder.appendDate(kFTDCCollectEndField, getCurrentDate(opCtx.get()));

    return std::tuple<BSONObj, Date_t>(builder.obj(), start);
}

SampleCollectorCache::SampleCollectorCache(Milliseconds maxSampleWaitMS,
                                           size_t minThreads,
                                           size_t maxThreads)
    : _maxSampleWaitMS(maxSampleWaitMS), _minThreads(minThreads), _maxThreads(maxThreads) {
    _startNewPool(minThreads, maxThreads);
}

SampleCollectorCache::~SampleCollectorCache() {
    stdx::lock_guard lk(_mutex);
    _shutdownPool_inlock(lk);
}

void SampleCollectorCache::addCollector(StringData name, bool hasData, SampleCollectFn&& fn) {
    _sampleCollectors[std::string{name}] = {
        ClientStrand::make(getGlobalServiceContext()->getService()->makeClient(
            std::string{name}, nullptr, ClientOperationKillableByStepdown{false})),
        boost::none,
        std::move(fn),
        0,
        hasData};
}

void SampleCollectorCache::refresh(OperationContext* opCtx, BSONObjBuilder* builder) {
    auto timeout = _maxSampleWaitMS.load();
    for (auto it = _sampleCollectors.begin(); it != _sampleCollectors.end(); it++) {
        auto& name = it->first;
        auto& collector = it->second;

        // Skip collection if this collector has no data to return
        if (!collector.hasData) {
            continue;
        }

        // Schedule a collection if one is not already running
        if (collector.updatedValue == boost::none) {
            auto [promise, future] = makePromiseFuture<BSONObj>();
            collector.updatedValue = std::move(future).semi();

            stdx::lock_guard lk(_mutex);
            _pool->schedule([it, promise = std::move(promise)](Status) mutable {
                BSONObjBuilder collectorBuilder;
                auto& collector = it->second;
                auto client = collector.clientStrand->bind();
                ServiceContext::UniqueOperationContext opCtxPtr;

                try {
                    opCtxPtr = client->makeOperationContext();
                    opCtxPtr->setEnforceConstraints(false);
                    auto collectionOpCtx = opCtxPtr.get();

                    ScopedAdmissionPriority<ExecutionAdmissionContext> admissionPriority(
                        collectionOpCtx, AdmissionContext::Priority::kExempt);

                    // Set a secondaryOk=true read preference because some collections run commands
                    // such as aggregation which are AllowedOnSecondary::kOptIn.
                    ReadPreferenceSetting::get(collectionOpCtx) =
                        ReadPreferenceSetting{ReadPreference::Nearest};

                    collectorBuilder.appendDate(kFTDCCollectStartField,
                                                getCurrentDate(collectionOpCtx));
                    collector.collectFn(collectionOpCtx, &collectorBuilder);
                    collectorBuilder.appendDate(kFTDCCollectEndField,
                                                getCurrentDate(collectionOpCtx));
                } catch (const DBException& e) {
                    LOGV2_WARNING(
                        10179600, "Collector threw an error", "collector"_attr = it->first);
                    promise.setError(e.toStatus());
                    return;
                }

                // Ensure the collector did not set a read timestamp.
                invariant(
                    shard_role_details::getRecoveryUnit(opCtxPtr.get())->getTimestampReadSource() ==
                    RecoveryUnit::ReadSource::kNoTimestamp);

                promise.emplaceValue(collectorBuilder.obj());
            });
        } else {
            LOGV2_DEBUG(10179601,
                        3,
                        "Skipping collection because it's still running",
                        "collector"_attr = name,
                        "timesSkipped"_attr = ++collector.timesSkipped);
        }

        // Wait for collector to finish or _maxSampleWaitMS, whichever comes first.
        BSONObj result;
        try {
            opCtx->runWithDeadline(
                getCurrentDate(opCtx) + timeout, ErrorCodes::ExceededTimeLimit, [&]() {
                    result = std::move(collector.updatedValue->get(opCtx));
                });
        } catch (const DBException& e) {
            if (e.code() == ErrorCodes::ExceededTimeLimit) {
                LOGV2_INFO(10179602,
                           "Collection timed out on collector",
                           "collector"_attr = name,
                           "timeout"_attr = timeout);
                continue;
            }
            // Service shutdown may cause the opCtx to be interrupted. This should not be process
            // fatal so we can swallow the error and stop collection.
            if (e.code() == ErrorCodes::InterruptedAtShutdown) {
                LOGV2_DEBUG(10179603, 4, "Interrupting collector wait due to shutdown");
                break;
            }

            throw;
        }

        uassert(ErrorCodes::BadValue,
                fmt::format("Expected result from collection of {}", name),
                !result.isEmpty());
        builder->append(name, result);
        collector.updatedValue = boost::none;
        collector.timesSkipped = 0;
    }
}

void SampleCollectorCache::_startNewPool(size_t minThreads, size_t maxThreads) {
    ThreadPool::Options options;
    options.poolName = "FTDCCollector";
    options.minThreads = minThreads;
    options.maxThreads = maxThreads;

    _pool = std::make_unique<ThreadPool>(std::move(options));
    _pool->startup();
}

void AsyncFTDCCollectorCollection::add(std::unique_ptr<FTDCCollectorInterface> collector) {
    auto collectFn = [collector = collector.get()](OperationContext* opCtx,
                                                   BSONObjBuilder* builder) {
        collector->collect(opCtx, *builder);
    };

    _collectorCache->addCollector(collector->name(), collector->hasData(), std::move(collectFn));
    _collectors.push_back(std::move(collector));
}

void AsyncFTDCCollectorCollection::_collect(OperationContext* opCtx, BSONObjBuilder* builder) {
    _collectorCache->refresh(opCtx, builder);
}

void SyncFTDCCollectorCollection::add(std::unique_ptr<FTDCCollectorInterface> collector) {
    // TODO: ensure the collectors all have unique names.
    _collectors.emplace_back(std::move(collector));
}

void SyncFTDCCollectorCollection::_collect(OperationContext* opCtx, BSONObjBuilder* builder) {
    for (auto& collector : _collectors) {
        // Skip collection if this collector has no data to return
        if (!collector->hasData()) {
            continue;
        }

        try {
            BSONObjBuilder subObjBuilder(builder->subobjStart(collector->name()));

            // Add a Date_t before and after each BSON is collected so that we can track timing of
            // the collector.
            subObjBuilder.appendDate(kFTDCCollectStartField, getCurrentDate(opCtx));
            collector->collect(opCtx, subObjBuilder);
            subObjBuilder.appendDate(kFTDCCollectEndField, getCurrentDate(opCtx));
        } catch (...) {
            LOGV2_ERROR(9761500,
                        "Collector threw an error",
                        "error"_attr = exceptionToStatus(),
                        "collector"_attr = collector->name());
            throw;
        }

        // Ensure the collector did not set a read timestamp.
        invariant(shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource() ==
                  RecoveryUnit::ReadSource::kNoTimestamp);
    }
}

}  // namespace mongo
