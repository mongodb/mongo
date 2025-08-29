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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#include <variant>

namespace mongo {
/**
 * Increments a counter by the time elapsed since its construction when it goes out of
 * scope.
 */
class ScopedTimer {
public:
    ScopedTimer();
    ScopedTimer(Nanoseconds* counter, ClockSource* cs);
    ScopedTimer(Nanoseconds* counter, TickSource* ts);
    ScopedTimer(ScopedTimer&&) noexcept;
    ScopedTimer& operator=(ScopedTimer&&) noexcept;
    ~ScopedTimer();

private:
    class State;
    class CsState;
    class TsState;
    std::unique_ptr<State> _state;
};

// This is the enum for a short description of the timed sections during mongod/mongos
// startup/shutdown and magic restore. The enum id is directly converted into a string when being
// logged, so any changes to the enum ids should be communicated to the relevant team.
#define MONGO_EXPAND_TIMED_SECTION_IDS(X)       \
    /* For startup: */                          \
    X(initAndListenTotal)                       \
    X(runMongosTotal)                           \
    X(setUpTransportLayer)                      \
    X(setUpPeriodicRunner)                      \
    X(setUpOCSP)                                \
    X(initSyncCrashRecovery)                    \
    X(standaloneClusterParams)                  \
    X(userAndRolesGraph)                        \
    X(waitForMajorityService)                   \
    X(configServerState)                        \
    X(clusterTimeKeysManager)                   \
    X(startUpReplCoord)                         \
    X(recoverChangeStream)                      \
    X(logStartupOptions)                        \
    X(startUpTransportLayer)                    \
    X(createFCVDocument)                        \
    X(restoreFCVDocument)                       \
    X(dropAbandonedIdents)                      \
    X(repairServerConfigNamespace)              \
    X(initializeFCV)                            \
    X(repairLocalDB)                            \
    X(repairRemainingDB)                        \
    X(initializeFCVForIndex)                    \
    X(createLockFile)                           \
    X(getStorageEngineMetadata)                 \
    X(validateMetadata)                         \
    X(createStorageEngine)                      \
    X(writePID)                                 \
    X(writeNewMetadata)                         \
    X(createSystemUsersIndex)                   \
    X(createSystemRolesIndex)                   \
    X(initializeFromShardIdentity)              \
    X(loadGlobalSettings)                       \
    X(initializeGlobalShardingState)            \
    X(resetConfigConnectionString)              \
    X(waitForSigningKeys)                       \
    X(preCacheRoutingInfo)                      \
    X(preWarmConnectionPool)                    \
    X(refreshBalancerConfig)                    \
    X(refreshRWConcernDefaults)                 \
    X(startMongosFTDC)                          \
    X(setUpScriptEngine)                        \
    X(initializeAuditSynchronizeJob)            \
    /* For shutdown: */                         \
    X(shutdownTaskTotal)                        \
    X(cleanupTaskTotal)                         \
    X(enterTerminalShutdown)                    \
    X(stepDownReplCoord)                        \
    X(quiesceMode)                              \
    X(stopFLECrud)                              \
    X(shutDownMirrorMaestro)                    \
    X(shutDownWaitForMajorityService)           \
    X(shutDownLogicalSessionCache)              \
    X(shutDownQueryAnalysisSampler)             \
    X(shutDownGlobalConnectionPool)             \
    X(shutDownSearchTaskExecutors)              \
    X(shutDownFlowControlTicketHolder)          \
    X(shutDownReplicaSetNodeExecutor)           \
    X(shutDownAbortExpiredTransactionsThread)   \
    X(shutDownRollbackUnderCachePressureThread) \
    X(shutDownIndexConsistencyChecker)          \
    X(killAllOperations)                        \
    X(shutDownOpenTransactions)                 \
    X(acquireRSTL)                              \
    X(shutDownIndexBuildsCoordinator)           \
    X(shutDownReplicaSetMonitor)                \
    X(shutDownShardRegistry)                    \
    X(shutDownTransactionCoord)                 \
    X(shutDownLogicalTimeValidator)             \
    X(shutDownExecutorPool)                     \
    X(shutDownCatalogCache)                     \
    X(shutDownTransportLayer)                   \
    X(shutDownHealthLog)                        \
    X(shutDownTTLMonitor)                       \
    X(shutDownExpiredDocumentRemover)           \
    X(shutDownOplogCapMaintainer)               \
    X(shutDownStorageEngine)                    \
    X(shutDownFTDC)                             \
    X(shutDownOCSP)                             \
    X(shutDownOtelMetrics)                      \
    X(shutDownReplicaSetAwareServices)          \
    X(waitForStartupComplete)                   \
    X(shutDownReplication)                      \
    X(shutDownInitialSyncer)                    \
    X(shutDownExternalState)                    \
    X(shutDownReplExecutor)                     \
    X(joinReplExecutor)                         \
    X(closeListenerSockets)                     \
    X(shutDownSynchronizeJob)                   \
    X(shutDownClusterParamRefresher)            \
    X(abortAllTransactions)                     \
    X(joinLogicalSessionCache)                  \
    X(shutDownCursorManager)                    \
    X(shutDownStateRequiredForStorageAccess)    \
    /* For magic restore: */                    \
    X(magicRestoreToolTotal)                    \
    X(readMagicRestoreConfig)                   \
    X(truncateOplogAndLocalDB)                  \
    X(insertOplogEntries)                       \
    X(truncateOplogToPIT)                       \
    X(applyOplogEntriesForRestore)              \
    X(createInternalCollections)                \
    X(updateShardingMetadata)                   \
    X(upsertAutomationCredentials)              \
    /* */

enum class TimedSectionId {
#define X(e) e,
    MONGO_EXPAND_TIMED_SECTION_IDS(X)
#undef X
};

StringData toString(TimedSectionId id);

/**
 * Appends the time elapsed since its construction to a BSON Object.
 */
class SectionScopedTimer {
public:
    SectionScopedTimer();

    /** If `builder` is null, the object is inactive and does nothing at all. */
    SectionScopedTimer(ClockSource* clockSource,
                       TimedSectionId timedSectionId,
                       BSONObjBuilder* builder);

    SectionScopedTimer(SectionScopedTimer&&) noexcept;
    SectionScopedTimer& operator=(SectionScopedTimer&&) noexcept;
    ~SectionScopedTimer();

private:
    class State;
    std::unique_ptr<State> _state;
};

}  // namespace mongo
