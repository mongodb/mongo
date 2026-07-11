// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/s/load_balancer_support.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/hello/hello_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/mongos_server_parameters_gen.h"
#include "mongo/platform/atomic.h"
#include "mongo/platform/compiler.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::load_balancer_support {
namespace {

MONGO_FAIL_POINT_DEFINE(loadBalancerSupportClientIsFromLoadBalancerPort);

struct PerService {
    /**
     * When a client reaches a mongos through a load balancer, the `serviceId`
     * identifies the mongos to which it is connected. It persists through the
     * lifespan of the service context.
     */
    OID serviceId = OID::gen();
};

class PerClient {
public:
    bool isLoadBalancerPeer() const;

    bool isConnectedToLoadBalancerPort() const;

    /**
     * Sets whether the connection is a loadBalanced connection
     * and whether it comes from a loadBalancerPort on the owning
     * session object.
     */
    void setIsLoadBalancerPeer(bool helloHasLoadBalancedOption);

    bool didHello() const {
        return _didHello;
    }

    void setDidHello() {
        _didHello = true;
    }

    LogicalSessionId mruLogicalSessionId() {
        return _lsid;
    }

    void setMruSession(LogicalSessionId lsid) {
        _lsid = lsid;
    }

private:
    /** True after we send this client a hello reply. */
    bool _didHello = false;

    /** Most recent LogicalSession used by the Client in a multi-statement txn. */
    LogicalSessionId _lsid;
};

const auto getPerServiceState = ServiceContext::declareDecoration<PerService>();
const auto getPerClientState = Client::declareDecoration<PerClient>();

bool PerClient::isLoadBalancerPeer() const {
    const auto& session = getPerClientState.owner(this)->session();

    return session && session->isLoadBalancerPeer();
}

bool PerClient::isConnectedToLoadBalancerPort() const {
    if (MONGO_unlikely(loadBalancerSupportClientIsFromLoadBalancerPort.shouldFail())) {
        return true;
    }
    const auto& session = getPerClientState.owner(this)->session();
    return session && session->isConnectedToLoadBalancerPort();
}

void PerClient::setIsLoadBalancerPeer(bool helloHasLoadBalancedOption) {
    if (const auto& session = getPerClientState.owner(this)->session()) {
        session->setIsLoadBalancerPeer(helloHasLoadBalancedOption);
    }
}

}  // namespace

bool isEnabled() {
    const auto val = loadBalancerPort.load();
    return val != 0 || MONGO_unlikely(loadBalancerSupportClientIsFromLoadBalancerPort.shouldFail());
}

boost::optional<int> getLoadBalancerPort() {
    auto val = loadBalancerPort.load();
    if (val != 0)
        return val;
    return {};
}

void handleHello(OperationContext* opCtx, BSONObjBuilder* result, bool helloHasLoadBalancedOption) {
    auto& perClient = getPerClientState(opCtx->getClient());
    if (perClient.didHello() || !perClient.isConnectedToLoadBalancerPort())
        return;

    perClient.setIsLoadBalancerPeer(helloHasLoadBalancedOption);

    if (helloHasLoadBalancedOption) {
        result->append(HelloCommandReply::kServiceIdFieldName,
                       getPerServiceState(opCtx->getServiceContext()).serviceId);
    }
    perClient.setDidHello();
}

bool isLoadBalancerPeer(Client* client) {
    return client->session() ? client->session()->isLoadBalancerPeer() : false;
}

LogicalSessionId getMruSession(Client* client) {
    return getPerClientState(client).mruLogicalSessionId();
}

void setMruSession(Client* client, LogicalSessionId id) {
    getPerClientState(client).setMruSession(id);
}
}  // namespace mongo::load_balancer_support
