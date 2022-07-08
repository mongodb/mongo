/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/s/load_balancer_support.h"

#include <memory>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/hello_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/s/mongos_server_parameters_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

namespace mongo::load_balancer_support {
namespace {

MONGO_FAIL_POINT_DEFINE(clientIsFromLoadBalancer);

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
    bool isFromLoadBalancer() const;

    void setIsFromLoadBalancer() {
        _isFromLoadBalancer = true;
    }

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
    /** True if the connection was established through a load balancer. */
    bool _isFromLoadBalancer = false;

    /** True after we send this client a hello reply. */
    bool _didHello = false;

    /** Most recent LogicalSession used by the Client in a multi-statement txn. */
    LogicalSessionId _lsid;
};

const auto getPerServiceState = ServiceContext::declareDecoration<PerService>();
const auto getPerClientState = Client::declareDecoration<PerClient>();

bool PerClient::isFromLoadBalancer() const {
    if (MONGO_unlikely(clientIsFromLoadBalancer.shouldFail())) {
        return true;
    }
    const auto& session = getPerClientState.owner(this)->session();

    return session && session->isFromLoadBalancer();
}

}  // namespace

bool isEnabled() {
    const auto val = loadBalancerPort.load();
    return val != 0 || MONGO_unlikely(clientIsFromLoadBalancer.shouldFail());
}

boost::optional<int> getLoadBalancerPort() {
    auto val = loadBalancerPort.load();
    if (val != 0)
        return val;
    return {};
}

void handleHello(OperationContext* opCtx, BSONObjBuilder* result, bool helloHasLoadBalancedOption) {
    auto& perClient = getPerClientState(opCtx->getClient());
    if (perClient.didHello() || !perClient.isFromLoadBalancer())
        return;

    uassert(ErrorCodes::LoadBalancerSupportMismatch,
            "The server is being accessed through a load balancer, but "
            "this driver does not have load balancing enabled",
            helloHasLoadBalancedOption);
    result->append(HelloCommandReply::kServiceIdFieldName,
                   getPerServiceState(opCtx->getServiceContext()).serviceId);
    perClient.setDidHello();
}

bool isFromLoadBalancer(Client* client) {
    return getPerClientState(client).isFromLoadBalancer();
}

LogicalSessionId getMruSession(Client* client) {
    return getPerClientState(client).mruLogicalSessionId();
}

void setMruSession(Client* client, LogicalSessionId id) {
    getPerClientState(client).setMruSession(id);
}
}  // namespace mongo::load_balancer_support
