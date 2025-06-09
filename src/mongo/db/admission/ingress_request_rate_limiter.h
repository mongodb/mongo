/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/admission/rate_limiter.h"

#include <cstddef>
#include <cstdint>

namespace mongo {

class IngressRequestRateLimiter {
public:
    IngressRequestRateLimiter();
    /**
     * Returns the reference to IngressRequestRateLimiter associated with the operation's service
     * context.
     */
    static IngressRequestRateLimiter& get(ServiceContext* opCtx);

    /**
     * Attempt to receive admission into the system. If the current rate of request admissions has
     * exceeded the configured rate limit and consumed the burst size, the operation will be
     * rejected with an error in the SystemOverloaded category.
     */
    // TODO: SERVER-104932 Remove exemption of command not subject to admission control, all
    //       requests must use the rate limiter
    Status admitRequest(OperationContext* opCtx, bool commandInvocationSubjectToAdmissionControl);

    /**
     * Adjusts the refresh rate of the rate limiter to 'refreshRatePerSec'.
     */
    void setAdmissionRatePerSec(std::int32_t refreshRatePerSec);

    /**
     * Adjusts the rate limiter's burst rate to 'burstSize'.
     */
    void setAdmissionBurstSize(std::int32_t burstSize);

    /**
     * Called automatically when the value of the server parameter ingressRequestAdmissionRatePerSec
     * changes value.
     */
    static Status onUpdateAdmissionRatePerSec(std::int32_t refreshRatePerSec);

    /**
     * Called automatically when the value of the server parameter ingressRequestAdmissionBurstSize
     * changes value.
     */
    static Status onUpdateAdmissionBurstSize(std::int32_t burstSize);

    /**
     * Reports the ingress admission rate limiter metrics.
     */
    void appendStats(BSONObjBuilder* bob) const;

private:
    admission::RateLimiter _rateLimiter;
};

}  // namespace mongo
