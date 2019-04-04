/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MERIZO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kStorage

#include "merizo/platform/basic.h"

#include "merizo/db/periodic_runner_job_decrease_snapshot_cache_pressure.h"

#include "merizo/db/client.h"
#include "merizo/db/operation_context.h"
#include "merizo/db/service_context.h"
#include "merizo/db/snapshot_window_options.h"
#include "merizo/db/snapshot_window_util.h"
#include "merizo/util/log.h"
#include "merizo/util/periodic_runner.h"

namespace merizo {

void startPeriodicThreadToDecreaseSnapshotHistoryCachePressure(ServiceContext* serviceContext) {
    // Enforce calling this function once, and only once.
    static bool firstCall = true;
    invariant(firstCall);
    firstCall = false;

    auto periodicRunner = serviceContext->getPeriodicRunner();
    invariant(periodicRunner);

    PeriodicRunner::PeriodicJob job(
        "startPeriodicThreadToDecreaseSnapshotHistoryCachePressure",
        [](Client* client) {
            try {
                // The opCtx destructor handles unsetting itself from the Client.
                // (The PeriodicRunnerASIO's Client must be reset before returning.)
                auto opCtx = client->makeOperationContext();

                SnapshotWindowUtil::decreaseTargetSnapshotWindowSize(opCtx.get());
            } catch (const DBException& ex) {
                if (!ErrorCodes::isShutdownError(ex.toStatus().code())) {
                    warning() << "Periodic task to check for and decrease cache pressure caused by "
                                 "maintaining too much snapshot history failed! Caused by: "
                              << ex.toStatus();
                }
            }
        },
        Seconds(snapshotWindowParams.checkCachePressurePeriodSeconds.load()));

    auto handle = periodicRunner->makeJob(std::move(job));
    handle->start();

    SnapshotWindowParams::observeCheckCachePressurePeriodSeconds
        .addObserver([handle = std::move(handle)](const auto& secs) {
            try {
                handle->setPeriod(Seconds(secs));
            } catch (const DBException& ex) {
                log() << "Failed to update period of thread which checks snapshot cache pressure "
                      << ex.toStatus();
            }
        });
}

}  // namespace merizo
