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

#include "mongo/db/admission/ingress_admission_rate_limiter.h"

#include "mongo/base/status.h"
#include "mongo/db/admission/ingress_admission_rate_limiter_gen.h"
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

const auto getIngressAdmissionRateLimiter =
    ServiceContext::declareDecoration<boost::optional<IngressAdmissionRateLimiter>>();

const ConstructorActionRegistererType<ServiceContext> onServiceContextCreate{
    "InitIngressAdmissionRateLimiter", [](ServiceContext* ctx) {
        getIngressAdmissionRateLimiter(ctx).emplace();
    }};
}  // namespace

IngressAdmissionRateLimiter::IngressAdmissionRateLimiter()
    : _rateLimiter{static_cast<double>(gIngressAdmissionRateLimiterRatePerSec.load()),
                   static_cast<double>(gIngressAdmissionRateLimiterBurstSize.load()),
                   0,
                   "IngressAdmissionRateLimiter"} {
    if (MONGO_unlikely(ingressRateLimiterVerySlowRate.shouldFail())) {
        _rateLimiter.setRefreshRatePerSec(kSlowest);
    }
}


IngressAdmissionRateLimiter& IngressAdmissionRateLimiter::get(ServiceContext* service) {
    return *getIngressAdmissionRateLimiter(service);
}

Status IngressAdmissionRateLimiter::admitRequest(OperationContext* opCtx) {
    // TODO: SERVER-104934 Implement ip based exemption

    return _rateLimiter.acquireToken(opCtx);
}

void IngressAdmissionRateLimiter::setAdmissionRatePerSec(std::int32_t refreshRatePerSec) {
    if (MONGO_unlikely(ingressRateLimiterVerySlowRate.shouldFail())) {
        _rateLimiter.setRefreshRatePerSec(kSlowest);
        return;
    }
    _rateLimiter.setRefreshRatePerSec(refreshRatePerSec);
}

void IngressAdmissionRateLimiter::setAdmissionBurstSize(std::int32_t burstSize) {
    _rateLimiter.setBurstSize(burstSize);
}

Status IngressAdmissionRateLimiter::onUpdateAdmissionRatePerSec(std::int32_t refreshRatePerSec) {
    if (auto client = Client::getCurrent()) {
        auto& instance = getIngressAdmissionRateLimiter(client->getServiceContext());

        if (MONGO_unlikely(ingressRateLimiterVerySlowRate.shouldFail())) {
            instance->_rateLimiter.setRefreshRatePerSec(kSlowest);
            return Status::OK();
        }

        instance->setAdmissionRatePerSec(refreshRatePerSec);
    }

    return Status::OK();
}

Status IngressAdmissionRateLimiter::onUpdateAdmissionBurstSize(std::int32_t burstSize) {
    if (auto client = Client::getCurrent()) {
        getIngressAdmissionRateLimiter(client->getServiceContext())
            ->setAdmissionBurstSize(static_cast<double>(burstSize));
    }

    return Status::OK();
}

}  // namespace mongo
