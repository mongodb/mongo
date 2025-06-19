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
#include "mongo/bson/json.h"
#include "mongo/db/admission/ingress_request_rate_limiter_gen.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/service_context.h"
#include "mongo/transport/cidr_range_list_parameter.h"
#include "mongo/util/decorable.h"

#include <boost/optional.hpp>


namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(ingressRateLimiterVerySlowRate);

VersionedValue<CIDRList> ingressRequestRateLimiterExemptions;

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


// TODO: SERVER-106468 Define CIDRRangeListParameter and remove this glue code
void IngressRequestRateLimiterExemptions::append(OperationContext*,
                                                 BSONObjBuilder* bob,
                                                 StringData name,
                                                 const boost::optional<TenantId>&) {
    transport::appendCIDRRangeListParameter(ingressRequestRateLimiterExemptions, bob, name);
}

Status IngressRequestRateLimiterExemptions::set(const BSONElement& value,
                                                const boost::optional<TenantId>&) {
    return transport::setCIDRRangeListParameter(ingressRequestRateLimiterExemptions, value.Obj());
}

Status IngressRequestRateLimiterExemptions::setFromString(StringData str,
                                                          const boost::optional<TenantId>&) {
    return transport::setCIDRRangeListParameter(ingressRequestRateLimiterExemptions, fromjson(str));
}

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

Status IngressRequestRateLimiter::admitRequest(Client* client) {
    // The rate limiter applies only requests when the client is authenticated to prevent DoS
    // attacks caused by many unauthenticated requests. In the case auth is disabled, all
    // requests will be subject to rate limiting.
    const auto authorizationSession = AuthorizationSession::get(client);

    const auto isConnectionExempt = [&] {
        ingressRequestRateLimiterExemptions.refreshSnapshot(_ingressRequestRateLimiterExemptions);

        return _ingressRequestRateLimiterExemptions &&
            client->session()->isExemptedByCIDRList(*_ingressRequestRateLimiterExemptions);
    };

    if ((!authorizationSession->shouldIgnoreAuthChecks() &&
         !authorizationSession->isAuthenticated()) ||
        isConnectionExempt()) {
        _rateLimiter.recordExemption();
        return Status::OK();
    }

    return _rateLimiter.tryAcquireToken();
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
