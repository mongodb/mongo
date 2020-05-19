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

#include "mongo/db/logical_time.h"
#include "mongo/platform/mutex.h"

namespace mongo {
class ServiceContext;
class OperationContext;


/**
 * LogicalClock provides a legacy interface to the cluster time, which is now provided by the
 * VectorClock class.
 *
 * TODO SERVER-48433: Remove this legacy LogicalClock interface.
 */
class LogicalClock {
public:
    // Decorate ServiceContext with LogicalClock instance.
    static LogicalClock* get(ServiceContext* service);
    static LogicalClock* get(OperationContext* ctx);
    static void set(ServiceContext* service, std::unique_ptr<LogicalClock> logicalClock);

    /**
     * Returns the current cluster time if this is a replica set node, otherwise returns a null
     * logical time.
     */
    static LogicalTime getClusterTimeForReplicaSet(ServiceContext* svcCtx);
    static LogicalTime getClusterTimeForReplicaSet(OperationContext* opCtx);

    /**
     * Creates an instance of LogicalClock.
     */
    LogicalClock(ServiceContext*);

    /**
     * Returns the current clusterTime.
     */
    LogicalTime getClusterTime();

private:
    ServiceContext* const _service;
};

}  // namespace mongo
