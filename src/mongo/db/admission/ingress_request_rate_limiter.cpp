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

#include "mongo/db/admission/ingress_request_rate_limiter.h"

#include "mongo/base/status.h"
#include "mongo/db/admission/ingress_request_rate_limiter_gen.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/service_context.h"
#include "mongo/util/decorable.h"

#include <limits>

#include <boost/optional.hpp>


namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(ingressRateLimiterVerySlowRate);

// This is very slow since it would allow one operation every 200,000 seconds, so roughly one
// every 55 hours
constexpr auto kSlowest = 5e-6;

const auto getIngressRequestRateLimiter =
    ServiceContext::declareDecoration<boost::optional<IngressRequestRateLimiter>>();

const ConstructorActionRegistererType<ServiceContext> onServiceContextCreate{
    "InitIngressRequestRateLimiter", [](ServiceContext* ctx) {
        getIngressRequestRateLimiter(ctx).emplace();
    }};
}  // namespace

IngressRequestRateLimiter::IngressRequestRateLimiter()
    : _rateLimiter{static_cast<double>(gIngressRequestRateLimiterRatePerSec.load()),
                   static_cast<double>(gIngressRequestRateLimiterBurstSize.load()),
                   0,
                   "ingressRequestRateLimiter"} {
    if (MONGO_unlikely(ingressRateLimiterVerySlowRate.shouldFail())) {
        _rateLimiter.updateRateParameters(kSlowest, gIngressRequestRateLimiterBurstSize.load());
    }
}


IngressRequestRateLimiter& IngressRequestRateLimiter::get(ServiceContext* service) {
    return *getIngressRequestRateLimiter(service);
}

Status IngressRequestRateLimiter::admitRequest(OperationContext* opCtx,
                                               bool commandInvocationSubjectToAdmissionControl) {
    // TODO: SERVER-104934 Implement ip based exemption
    // TODO: SERVER-104932 Remove commandInvocationSubjectToAdmissionControl in favor of
    // a failpoint to bypass operations such as shutdown and setParameter

    // The rate limiter applies only requests when the client is authenticated to prevent DoS
    // attacks caused by many unauthenticated requests. In the case auth is disabled, all
    // requests will be subject to rate limiting.
    if (!AuthorizationSession::get(opCtx->getClient())->isAuthenticated() ||
        !commandInvocationSubjectToAdmissionControl) {
        _rateLimiter.recordExemption();
        return Status::OK();
    }

    return _rateLimiter.acquireToken(opCtx);
}

void IngressRequestRateLimiter::setAdmissionRatePerSec(std::int32_t refreshRatePerSec) {
    if (MONGO_unlikely(ingressRateLimiterVerySlowRate.shouldFail())) {
        _rateLimiter.updateRateParameters(kSlowest, gIngressRequestRateLimiterBurstSize.load());
        return;
    }
    _rateLimiter.updateRateParameters(refreshRatePerSec,
                                      gIngressRequestRateLimiterBurstSize.load());
}

void IngressRequestRateLimiter::setAdmissionBurstSize(std::int32_t burstSize) {
    _rateLimiter.updateRateParameters(gIngressRequestRateLimiterRatePerSec.load(), burstSize);
}

Status IngressRequestRateLimiter::onUpdateAdmissionRatePerSec(std::int32_t refreshRatePerSec) {
    if (auto client = Client::getCurrent()) {
        auto& instance = getIngressRequestRateLimiter(client->getServiceContext());

        if (MONGO_unlikely(ingressRateLimiterVerySlowRate.shouldFail())) {
            instance->_rateLimiter.updateRateParameters(kSlowest,
                                                        gIngressRequestRateLimiterBurstSize.load());
            return Status::OK();
        }

        instance->setAdmissionRatePerSec(refreshRatePerSec);
    }

    return Status::OK();
}

Status IngressRequestRateLimiter::onUpdateAdmissionBurstSize(std::int32_t burstSize) {
    if (auto client = Client::getCurrent()) {
        getIngressRequestRateLimiter(client->getServiceContext())
            ->setAdmissionBurstSize(static_cast<double>(burstSize));
    }

    return Status::OK();
}

void IngressRequestRateLimiter::appendStats(BSONObjBuilder* bob) const {
    // First we get the stats in a separate object in order to not mutate bob
    auto rateLimiterBob = BSONObjBuilder{};
    _rateLimiter.appendStats(&rateLimiterBob);
    auto const rateLimiterStats = rateLimiterBob.obj();

    // Then we copy elements one by one to avoid coping queueing stats
    bob->append(rateLimiterStats.getField("rejectedAdmissions"));
    bob->append(rateLimiterStats.getField("successfulAdmissions"));
    bob->append(rateLimiterStats.getField("exemptedAdmissions"));
    bob->append(rateLimiterStats.getField("attemptedAdmissions"));
    bob->append(rateLimiterStats.getField("totalAvailableTokens"));
}

}  // namespace mongo
