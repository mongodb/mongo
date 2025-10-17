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
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/transport/cidr_range_list_parameter.h"
#include "mongo/util/decorable.h"

#include <boost/optional.hpp>


namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(ingressRequestRateLimiterFractionalRateOverride);

VersionedValue<CIDRList> ingressRequestRateLimiterIPExemptions;
VersionedValue<std::vector<std::string>> ingressRequestRateLimiterAppExemptions;

const auto getIngressRequestRateLimiter =
    ServiceContext::declareDecoration<boost::optional<IngressRequestRateLimiter>>();

const ConstructorActionRegistererType<ServiceContext> onServiceContextCreate{
    "InitIngressRequestRateLimiter", [](ServiceContext* ctx) {
        getIngressRequestRateLimiter(ctx).emplace();
    }};

class ClientAdmissionControlState {
public:
    static bool isExempted(Client*);

private:
    template <typename T>
    using Snapshot = VersionedValue<T>::Snapshot;

    bool _areSnapshotsCurrent() const {
        return ingressRequestRateLimiterIPExemptions.isCurrent(_ipExemptions) &&
            ingressRequestRateLimiterAppExemptions.isCurrent(_appExemptions);
    }

    bool _isAuthorizationExempt(Client* client) {
        if (!AuthorizationSession::exists(client)) {
            return true;
        }

        const auto authorizationSession = AuthorizationSession::get(client);
        return !authorizationSession->shouldIgnoreAuthChecks() &&
            !authorizationSession->isAuthenticated();
    };

    bool _isConnectionExempt(Client* client) {
        ingressRequestRateLimiterIPExemptions.refreshSnapshot(_ipExemptions);
        return _ipExemptions && client->session()->isExemptedByCIDRList(*_ipExemptions);
    }

    bool _isApplicationExempt(Client* client) {
        const auto clientMetadata = ClientMetadata::get(client);
        if (!clientMetadata) {
            return false;
        }

        // Don't refresh the client's snapshot of app exemptions until we've seen the client
        // metadata, to ensure that we don't cache an outdated exempted status based on non-existent
        // client metadata.
        ingressRequestRateLimiterAppExemptions.refreshSnapshot(_appExemptions);
        if (!_appExemptions) {
            return false;
        }

        const auto& driverName = clientMetadata->getDriverName();
        const auto& applicationName = clientMetadata->getApplicationName();
        for (auto& appExemption : *_appExemptions) {
            if (driverName.starts_with(appExemption) || applicationName.starts_with(appExemption)) {
                return true;
            }
        }
        return false;
    }

    bool _isExempted(Client* client) {
        // The rate limiter applies only requests when the client is authenticated to prevent DoS
        // attacks caused by many unauthenticated requests. In the case auth is disabled, all
        // requests will be subject to rate limiting.
        if (_isAuthorizationExempt(client)) {
            return true;
        }

        if (MONGO_unlikely(!_exempted.has_value() || !_areSnapshotsCurrent())) {
            _exempted = _isConnectionExempt(client) || _isApplicationExempt(client);
        }
        return *_exempted;
    }

    Snapshot<CIDRList> _ipExemptions;
    Snapshot<std::vector<std::string>> _appExemptions;
    boost::optional<bool> _exempted;
};

const auto getAdmissionControlState = Client::declareDecoration<ClientAdmissionControlState>();

bool ClientAdmissionControlState::isExempted(Client* client) {
    return getAdmissionControlState(client)._isExempted(client);
}

std::shared_ptr<std::vector<std::string>> parseApplicationExemptionList(const BSONObj& b) {
    IDLParserContext ctx("ApplicationExemptionListParameters");
    const auto params = ApplicationExemptionListParameters::parse(b, ctx);

    auto apps = std::make_shared<std::vector<std::string>>();
    for (const auto& app : params.getAppNames()) {
        apps->emplace_back(std::string{app});
    }

    return apps;
}

}  // namespace


// TODO: SERVER-106468 Define CIDRRangeListParameter and remove this glue code
void IngressRequestRateLimiterIPExemptions::append(OperationContext*,
                                                   BSONObjBuilder* bob,
                                                   StringData name,
                                                   const boost::optional<TenantId>&) {
    transport::appendCIDRRangeListParameter(ingressRequestRateLimiterIPExemptions, bob, name);
}

Status IngressRequestRateLimiterIPExemptions::set(const BSONElement& value,
                                                  const boost::optional<TenantId>&) {
    return transport::setCIDRRangeListParameter(ingressRequestRateLimiterIPExemptions, value.Obj());
}

Status IngressRequestRateLimiterIPExemptions::setFromString(StringData str,
                                                            const boost::optional<TenantId>&) {
    return transport::setCIDRRangeListParameter(ingressRequestRateLimiterIPExemptions,
                                                fromjson(str));
}

void IngressRequestRateLimiterAppExemptions::append(OperationContext*,
                                                    BSONObjBuilder* bob,
                                                    StringData name,
                                                    const boost::optional<TenantId>&) {
    auto snapshot = ingressRequestRateLimiterAppExemptions.makeSnapshot();

    if (snapshot) {
        BSONArrayBuilder bb(bob->subarrayStart(name));
        for (const auto& appName : *snapshot) {
            bb << appName;
        }
    } else {
        *bob << name << BSONNULL;
    }
}

Status IngressRequestRateLimiterAppExemptions::set(const BSONElement& value,
                                                   const boost::optional<TenantId>&) {
    ingressRequestRateLimiterAppExemptions.update(parseApplicationExemptionList(value.Obj()));
    return Status::OK();
}

Status IngressRequestRateLimiterAppExemptions::setFromString(StringData str,
                                                             const boost::optional<TenantId>&) {
    ingressRequestRateLimiterAppExemptions.update(parseApplicationExemptionList(fromjson(str)));
    return Status::OK();
}

IngressRequestRateLimiter::IngressRequestRateLimiter()
    : _rateLimiter{static_cast<double>(gIngressRequestRateLimiterRatePerSec.load()),
                   gIngressRequestRateLimiterBurstCapacitySecs.load(),
                   0,
                   "ingressRequestRateLimiter"} {
    if (const auto scopedFp = ingressRequestRateLimiterFractionalRateOverride.scoped();
        MONGO_unlikely(scopedFp.isActive())) {
        const auto rate = scopedFp.getData().getField("rate").numberDouble();
        _rateLimiter.updateRateParameters(rate, gIngressRequestRateLimiterBurstCapacitySecs.load());
    }
}


IngressRequestRateLimiter& IngressRequestRateLimiter::get(ServiceContext* service) {
    return *getIngressRequestRateLimiter(service);
}

Status IngressRequestRateLimiter::admitRequest(Client* client) {
    if (ClientAdmissionControlState::isExempted(client)) {
        _rateLimiter.recordExemption();
        return Status::OK();
    }

    auto rateLimitResult = _rateLimiter.tryAcquireToken();
    if (MONGO_unlikely(rateLimitResult == admission::RateLimiter::kRejectedErrorCode)) {
        return Status{ErrorCodes::IngressRequestRateLimitExceeded, rateLimitResult.reason()};
    }

    return rateLimitResult;
}

void IngressRequestRateLimiter::updateRateParameters(double refreshRatePerSec,
                                                     double burstCapacitySecs) {
    if (const auto scopedFp = ingressRequestRateLimiterFractionalRateOverride.scoped();
        MONGO_unlikely(scopedFp.isActive())) {
        const auto rate = scopedFp.getData().getField("rate").numberDouble();
        _rateLimiter.updateRateParameters(rate, burstCapacitySecs);
        return;
    }
    _rateLimiter.updateRateParameters(refreshRatePerSec, burstCapacitySecs);
}

Status IngressRequestRateLimiter::onUpdateAdmissionRatePerSec(std::int32_t refreshRatePerSec) {
    if (auto client = Client::getCurrent()) {
        auto& instance = getIngressRequestRateLimiter(client->getServiceContext());
        instance->updateRateParameters(refreshRatePerSec,
                                       gIngressRequestRateLimiterBurstCapacitySecs.load());
    }

    return Status::OK();
}

Status IngressRequestRateLimiter::onUpdateAdmissionBurstCapacitySecs(double burstCapacitySecs) {
    if (auto client = Client::getCurrent()) {
        getIngressRequestRateLimiter(client->getServiceContext())
            ->updateRateParameters(gIngressRequestRateLimiterRatePerSec.load(), burstCapacitySecs);
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
