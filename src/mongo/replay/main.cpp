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

#include "mongo/base/initializer.h"
#include "mongo/db/service_context.h"
#include "mongo/db/wire_version.h"
#include "mongo/replay/config_handler.h"
#include "mongo/replay/replay_client.h"
#include "mongo/transport/asio/asio_session_manager.h"
#include "mongo/transport/asio/asio_transport_layer.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/version.h"

void setTransportLayer(mongo::ServiceContext* serviceContext) {
    using namespace mongo;
    transport::AsioTransportLayer::Options opts;
    opts.mode = transport::AsioTransportLayer::Options::kEgress;
    auto sm = std::make_unique<transport::AsioSessionManager>(serviceContext);
    auto tl = std::make_unique<transport::AsioTransportLayer>(opts, std::move(sm));
    serviceContext->setTransportLayerManager(
        std::make_unique<transport::TransportLayerManagerImpl>(std::move(tl)));
    auto tlPtr = serviceContext->getTransportLayerManager();
    uassertStatusOK(tlPtr->setup());
    uassertStatusOK(tlPtr->start());
}

void setMongoWireProtocol(mongo::ServiceContext* serviceContext) {
    using namespace mongo;
    // (Generic FCV reference): Initialize FCV.
    serverGlobalParams.mutableFCV.setVersion(mongo::multiversion::GenericFCV::kLatest);
    WireSpec::Specification spec;
    spec.incomingInternalClient.minWireVersion = RELEASE_2_4_AND_BEFORE;
    spec.incomingInternalClient.maxWireVersion = LATEST_WIRE_VERSION;
    spec.outgoing.minWireVersion = SUPPORTS_OP_MSG;
    spec.outgoing.maxWireVersion = LATEST_WIRE_VERSION;
    spec.isInternalClient = false;  // very important the connection cannot be viewed as internal.
    mongo::WireSpec::getWireSpec(serviceContext).initialize(std::move(spec));
}

int main(int argc, char** argv) {
    using namespace mongo;
    runGlobalInitializersOrDie(std::vector<std::string>(argv, argv + argc));
    setGlobalServiceContext(ServiceContext::make());
    auto serviceContext = getGlobalServiceContext();
    setTransportLayer(serviceContext);
    setMongoWireProtocol(serviceContext);
    ReplayClient replayClient;
    ConfigHandler configHandler;
    const auto& replayConfig = configHandler.parse(argc, argv);
    replayClient.replayRecording(replayConfig);
    uassertStatusOK(runGlobalDeinitializers());
    return 0;
}
