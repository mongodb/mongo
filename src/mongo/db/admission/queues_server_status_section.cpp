/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/admission/ingress_admission_control_gen.h"
#include "mongo/db/admission/ingress_admission_controller.h"
#include "mongo/db/admission/ticketholder_manager.h"
#include "mongo/db/commands/server_status.h"
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
        auto ticketHolderManager = TicketHolderManager::get(opCtx->getServiceContext());
        if (ticketHolderManager && role.has(ClusterRole::ShardServer)) {
            BSONObjBuilder executionBuilder(admissionBuilder.subobjStart("execution"));
            ticketHolderManager->appendStats(executionBuilder);
            executionBuilder.done();
        }

        if (gIngressAdmissionControlEnabled.load() && role.has(ClusterRole::ShardServer)) {
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

        return admissionBuilder.obj();
    }
};

auto& admissionSection =
    *ServerStatusSectionBuilder<AdmissionServerStatusSection>("queues").forShard().forRouter();

}  // namespace
}  // namespace admission
}  // namespace mongo
