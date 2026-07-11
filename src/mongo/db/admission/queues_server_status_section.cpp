// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/execution_control/ticketing_system.h"
#include "mongo/db/admission/ingress_admission_control_gen.h"
#include "mongo/db/admission/ingress_admission_controller.h"
#include "mongo/db/admission/write_throttler.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/transport/session_establishment_rate_limiter.h"
#include "mongo/transport/transport_layer.h"

namespace mongo {
namespace admission {
namespace {

class AdmissionServerStatusSection : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        const ClusterRole role = opCtx->getService()->role();

        BSONObjBuilder admissionBuilder;
        auto ticketingSystem =
            admission::execution_control::TicketingSystem::get(opCtx->getServiceContext());
        if (ticketingSystem && role.has(ClusterRole::ShardServer)) {
            BSONObjBuilder executionBuilder(admissionBuilder.subobjStart("execution"));
            ticketingSystem->appendStats(executionBuilder);
            executionBuilder.done();
        }

        if (gIngressAdmissionControlEnabled.load() || gFeatureFlagIngressRateLimiting.isEnabled()) {
            BSONObjBuilder ingressBuilder(admissionBuilder.subobjStart("ingress"));
            auto& controller = IngressAdmissionController::get(opCtx);
            controller.appendStats(ingressBuilder);
            ingressBuilder.done();
        }

        if (auto* limiter = transport::SessionEstablishmentRateLimiter::get(
                *opCtx->getServiceContext(), transport::TransportProtocol::MongoRPC)) {
            BSONObjBuilder ingressSessionEstablishmentBuilder(
                admissionBuilder.subobjStart("ingressSessionEstablishment"));
            limiter->appendStatsQueues(&ingressSessionEstablishmentBuilder);
            ingressSessionEstablishmentBuilder.done();
        }

        if ((role.has(ClusterRole::None) || role.has(ClusterRole::ShardServer))) {
            if (auto* throttler = WriteThrottler::get(opCtx)) {
                BSONObjBuilder writeThrottlerBuilder(
                    admissionBuilder.subobjStart("writeThrottler"));
                writeThrottlerBuilder.appendElements(throttler->generateSection());
                writeThrottlerBuilder.done();
            }
        }

        return admissionBuilder.obj();
    }
};

auto& admissionSection =
    *ServerStatusSectionBuilder<AdmissionServerStatusSection>("queues").forShard().forRouter();

}  // namespace
}  // namespace admission
}  // namespace mongo
