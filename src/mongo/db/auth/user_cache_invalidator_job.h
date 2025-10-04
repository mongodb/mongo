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

#include "mongo/base/status.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/service_context.h"
#include "mongo/util/duration.h"
#include "mongo/util/periodic_runner.h"

#include <memory>
#include <variant>

namespace mongo {

class AuthorizationManager;
class OperationContext;

/**
 * Background job that runs only in mongos and periodically checks in with the config servers
 * to determine whether any authorization information has changed, and if so causes the
 * AuthorizationManager to throw out its in-memory cache of User objects (which contains the
 * users' credentials, roles, privileges, etc).
 */
class UserCacheInvalidator {
public:
    using OIDorTimestamp = std::variant<OID, Timestamp>;

    UserCacheInvalidator(AuthorizationManager* authzManager);

    /**
     * Create a new UserCacheInvalidator as a decorator on the service context
     * and start the background job.
     */
    static void start(ServiceContext* serviceCtx, OperationContext* opCtx);

    /**
     * Waits for the job to complete and stops the thread.
     */
    static void stop(ServiceContext* serviceCtx);

    /**
     * Set the period of the background job. This should only be used internally (by the
     * setParameter).
     */
    void setPeriod(Milliseconds period);

private:
    void initialize(OperationContext* opCtx);
    void run();

    std::unique_ptr<PeriodicJobAnchor> _job;

    AuthorizationManager* const _authzManager;
    OIDorTimestamp _previousGeneration;
};

Status userCacheInvalidationIntervalSecsNotify(const int& newValue);

}  // namespace mongo
