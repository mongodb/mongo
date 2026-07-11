// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#include <string_view>

[[MONGO_MOD_PUBLIC]];

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
    X(shutDownFlowControl)                      \
    X(shutDownFTDC)                             \
    X(shutDownOCSP)                             \
    X(shutDownOtelMetrics)                      \
    X(shutDownOtelTraces)                       \
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
    X(closeLockFile)                            \
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
    X(shutDownAndJoinReadWriteConcernDefaults)  \
    X(shutDownRouterUptimeReporter)             \
    /* */

enum class TimedSectionId {
#define X(e) e,
    MONGO_EXPAND_TIMED_SECTION_IDS(X)
#undef X
};

std::string_view toString(TimedSectionId id);

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
