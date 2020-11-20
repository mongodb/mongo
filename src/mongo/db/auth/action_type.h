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

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"

namespace mongo {

/**
 * List describing the ActionTypes that should be created.
 * Please note that the order of the elements is not guaranteed to be the same across versions.
 * This means that the integer value assigned to each ActionType and used internally in ActionSet
 * also may change between versions.
 *
 * Actions marked "ID only" are not used for permission checks, but to identify events in logs.
 *
 * The `X` parameter is expanded into `X(action)` for each ActionType name in the list.
 * (https://en.wikipedia.org/wiki/X_Macro).
 */
#define EXPAND_ACTION_TYPE(X)                                                         \
    X(addShard)                                                                       \
    X(advanceClusterTime)                                                             \
    X(anyAction) /* Special ActionType that represents *all* actions */               \
    X(appendOplogNote)                                                                \
    X(applicationMessage)                                                             \
    X(auditLogRotate) /* ID only */                                                   \
    X(authCheck)      /* ID only */                                                   \
    X(authenticate)   /* ID only */                                                   \
    X(authSchemaUpgrade)                                                              \
    X(bypassDocumentValidation)                                                       \
    X(changeCustomData)                                                               \
    X(changePassword)                                                                 \
    X(changeOwnPassword)                                                              \
    X(changeOwnCustomData)                                                            \
    X(changeStream)                                                                   \
    X(checkFreeMonitoringStatus)                                                      \
    X(cleanupOrphaned)                                                                \
    X(clearJumboFlag)                                                                 \
    X(closeAllDatabases) /* Deprecated (backwards compatibility) */                   \
    X(collMod)                                                                        \
    X(collStats)                                                                      \
    X(compact)                                                                        \
    X(connPoolStats)                                                                  \
    X(connPoolSync)                                                                   \
    X(convertToCapped)                                                                \
    X(cpuProfiler)                                                                    \
    X(createCollection)                                                               \
    X(createDatabase) /* ID only */                                                   \
    X(createIndex)    /* ID only */                                                   \
    X(createRole)                                                                     \
    X(createUser)                                                                     \
    X(dbHash)                                                                         \
    X(dbStats)                                                                        \
    X(dropAllRolesFromDatabase) /* ID only */                                         \
    X(dropAllUsersFromDatabase) /* ID only */                                         \
    X(dropCollection)                                                                 \
    X(dropConnections)                                                                \
    X(dropDatabase)                                                                   \
    X(dropIndex)                                                                      \
    X(dropRole)                                                                       \
    X(dropUser)                                                                       \
    X(emptycapped)                                                                    \
    X(enableProfiler)                                                                 \
    X(enableSharding)                                                                 \
    X(exportCollection)                                                               \
    X(find)                                                                           \
    X(flushRouterConfig)                                                              \
    X(forceUUID)                                                                      \
    X(fsync)                                                                          \
    X(getDatabaseVersion)                                                             \
    X(getDefaultRWConcern)                                                            \
    X(getCmdLineOpts)                                                                 \
    X(getLog)                                                                         \
    X(getParameter)                                                                   \
    X(getShardMap)                                                                    \
    X(getShardVersion)                                                                \
    X(grantRole)                                                                      \
    X(grantPrivilegesToRole) /* ID only */                                            \
    X(grantRolesToRole)      /* ID only */                                            \
    X(grantRolesToUser)      /* ID only */                                            \
    X(hostInfo)                                                                       \
    X(impersonate)                                                                    \
    X(importCollection)                                                               \
    X(indexStats)                                                                     \
    X(inprog)                                                                         \
    X(insert)                                                                         \
    X(internal) /* Special action type that represents internal actions */            \
    X(invalidateUserCache)                                                            \
    X(killAnyCursor)                                                                  \
    X(killAnySession)                                                                 \
    X(killCursors) /* Deprecated in favor of killAnyCursor */                         \
    X(killop)                                                                         \
    X(listCachedAndActiveUsers)                                                       \
    X(listCollections)                                                                \
    X(listCursors)                                                                    \
    X(listDatabases)                                                                  \
    X(listIndexes)                                                                    \
    X(listSessions)                                                                   \
    X(listShards)                                                                     \
    X(logRotate)                                                                      \
    X(moveChunk)                                                                      \
    X(netstat)                                                                        \
    X(operationMetrics)                                                               \
    X(planCacheIndexFilter) /* view/update index filters */                           \
    X(planCacheRead)        /* view contents of plan cache */                         \
    X(planCacheWrite)       /* clear cache, drop cache entry, pin/unpin/shun plans */ \
    X(refineCollectionShardKey)                                                       \
    X(reIndex)                                                                        \
    X(remove)                                                                         \
    X(removeShard)                                                                    \
    X(renameCollection) /* ID only */                                                 \
    X(renameCollectionSameDB)                                                         \
    X(repairDatabase) /* Deprecated (backwards compatibility) */                      \
    X(replSetConfigure)                                                               \
    X(replSetGetConfig)                                                               \
    X(replSetGetStatus)                                                               \
    X(replSetHeartbeat)                                                               \
    X(replSetReconfig) /* ID only */                                                  \
    X(replSetResizeOplog)                                                             \
    X(replSetStateChange)                                                             \
    X(reshardCollection)                                                              \
    X(resync)                                                                         \
    X(revokeRole)                                                                     \
    X(revokePrivilegesFromRole) /* ID only */                                         \
    X(revokeRolesFromRole)      /* ID only */                                         \
    X(revokeRolesFromUser)      /* ID only */                                         \
    X(rotateCertificates)                                                             \
    X(runAsLessPrivilegedUser)                                                        \
    X(serverStatus)                                                                   \
    X(setAuthenticationRestriction)                                                   \
    X(setDefaultRWConcern)                                                            \
    X(setFeatureCompatibilityVersion)                                                 \
    X(setFreeMonitoring)                                                              \
    X(setParameter)                                                                   \
    X(shardCollection) /* ID only */                                                  \
    X(shardingState)                                                                  \
    X(shutdown)                                                                       \
    X(splitChunk)                                                                     \
    X(splitVector)                                                                    \
    X(storageDetails)                                                                 \
    X(top)                                                                            \
    X(touch)                                                                          \
    X(trafficRecord)                                                                  \
    X(unlock)                                                                         \
    X(useUUID)                                                                        \
    X(update)                                                                         \
    X(updateRole) /* ID only */                                                       \
    X(updateUser) /* ID only */                                                       \
    X(validate)                                                                       \
    X(viewRole)                                                                       \
    X(viewUser)                                                                       \
    /**/

enum class ActionType : uint32_t {
#define X_(a) a,
    EXPAND_ACTION_TYPE(X_)
#undef X_
};

#define X_(a) +1  // just count them
static constexpr uint32_t kNumActionTypes = 0 EXPAND_ACTION_TYPE(X_);
#undef X_

StatusWith<ActionType> parseActionFromString(StringData action);
StringData toStringData(ActionType a);
std::string toString(ActionType a);
std::ostream& operator<<(std::ostream& os, const ActionType& a);

}  // namespace mongo
