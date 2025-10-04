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

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/admission/ingress_request_rate_limiter.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostname_canonicalization.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_types.h"

#include <memory>

namespace mongo {
namespace {

// some universal sections

class Network : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder b;
        networkCounter.append(b);
        appendMessageCompressionStats(&b);


        if (gFeatureFlagIngressRateLimiting.isEnabled()) {
            appendIngressRequestRateLimiterStats(&b, opCtx->getServiceContext());
        }

        auto svcCtx = opCtx->getServiceContext();

        {
            BSONObjBuilder section = b.subobjStart("serviceExecutors");
            transport::ServiceExecutor::appendAllServerStats(&section, svcCtx);
        }

        if (auto tl = svcCtx->getTransportLayerManager())
            tl->appendStatsForServerStatus(&b);

        return b.obj();
    }

    void appendIngressRequestRateLimiterStats(BSONObjBuilder* b, ServiceContext* service) const {
        auto ingressRequestRateLimiterBuilder =
            BSONObjBuilder{b->subobjStart("ingressRequestRateLimiter")};
        const auto& ingressRequestRateLimeter = IngressRequestRateLimiter::get(service);
        ingressRequestRateLimeter.appendStats(&ingressRequestRateLimiterBuilder);
        ingressRequestRateLimiterBuilder.done();
    }
};
auto& network = *ServerStatusSectionBuilder<Network>("network");

class AdvisoryHostFQDNs final : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return false;
    }

    void appendSection(OperationContext* opCtx,
                       const BSONElement& configElement,
                       BSONObjBuilder* out) const override {
        auto statusWith =
            getHostFQDNs(getHostNameCached(), HostnameCanonicalizationMode::kForwardAndReverse);
        if (statusWith.isOK()) {
            out->append("advisoryHostFQDNs", statusWith.getValue());
        }
    }
};
// Register one instance of the section shared by both roles; the system has one set of FQDNs.
auto& advisoryHostFQDNs =
    *ServerStatusSectionBuilder<AdvisoryHostFQDNs>("advisoryHostFQDNs").forShard().forRouter();

}  // namespace
}  // namespace mongo
