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

#include "mongo/db/operation_context.h"

namespace mongo {

/**
 * Diagnostic logging of sharding metadata events (changelog and actionlog).
 */
class ShardingLogging {

public:
    ShardingLogging();
    ~ShardingLogging();

    /**
     * Retrieves the ShardingLogging instance associated with the current service/operation context.
     */
    static ShardingLogging* get(ServiceContext* serviceContext);
    static ShardingLogging* get(OperationContext* operationContext);

    Status logAction(OperationContext* opCtx,
                     const StringData what,
                     const StringData ns,
                     const BSONObj& detail);

    Status logChangeChecked(OperationContext* opCtx,
                            const StringData what,
                            const StringData ns,
                            const BSONObj& detail,
                            const WriteConcernOptions& writeConcern);

    void logChange(OperationContext* const opCtx,
                   const StringData what,
                   const StringData ns,
                   const BSONObj& detail,
                   const WriteConcernOptions& writeConcern) {
        // It is safe to ignore the results of `logChangeChecked` in many cases, as the
        // failure to log a change is often of no consequence.
        logChangeChecked(opCtx, what, ns, detail, writeConcern).ignore();
    }

private:
    /**
     * Creates the specified collection name in the config database.
     */
    Status _createCappedConfigCollection(OperationContext* opCtx,
                                         StringData collName,
                                         int cappedSize,
                                         const WriteConcernOptions& writeConcern);

    /**
     * Best effort method, which logs diagnostic events on the config server. If the config server
     * write fails for any reason a warning will be written to the local service log and the method
     * will return a failed status.
     *
     * @param opCtx Operation context in which the call is running
     * @param logCollName Which config collection to write to (excluding the database name)
     * @param what E.g. "split", "migrate" (not interpreted)
     * @param operationNS To which collection the metadata change is being applied (not interpreted)
     * @param detail Additional info about the metadata change (not interpreted)
     * @param writeConcern Write concern options to use for logging
     */
    Status _log(OperationContext* opCtx,
                const StringData logCollName,
                const StringData what,
                const StringData operationNSS,
                const BSONObj& detail,
                const WriteConcernOptions& writeConcern);

    // Member variable properties:
    // (S) Self-synchronizing; access in any way from any context.

    // Whether the logAction call should attempt to create the actionlog collection
    AtomicWord<int> _actionLogCollectionCreated{0};  // (S)

    // Whether the logChange call should attempt to create the changelog collection
    AtomicWord<int> _changeLogCollectionCreated{0};  // (S)
};

}  // namespace mongo
