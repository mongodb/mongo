// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
