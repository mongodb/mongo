// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/service_context.h"
#include "mongo/transport/asio/asio_transport_layer.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#ifdef MONGO_CONFIG_GRPC
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/transport/grpc/grpc_transport_layer_impl.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/version.h"
#endif

namespace mongo {
namespace {
// Linking with this file will configure an egress-only TransportLayer on all new ServiceContexts.
// Use this for integration tests that require only egress networking.

#ifdef MONGO_CONFIG_GRPC
std::unique_ptr<transport::TransportLayer> createGRPCTransportLayer(ServiceContext* sc) {
    transport::grpc::GRPCTransportLayer::Options opts;
    BSONObjBuilder bob;
    auto versionString =
        VersionInfoInterface::instance(VersionInfoInterface::NotEnabledAction::kFallback).version();
    uassertStatusOK(ClientMetadata::serialize(
        "MongoDB Internal Client", versionString, "Test-GRPCClient", &bob));
    auto metadataDoc = bob.obj();

    opts.clientMetadata = metadataDoc.getObjectField(kMetadataDocumentName).getOwned();
    opts.enableEgress = true;
    opts.enableIngress = false;

    auto runner = makePeriodicRunner(sc);
    sc->setPeriodicRunner(std::move(runner));

    return std::make_unique<transport::grpc::GRPCTransportLayerImpl>(sc, std::move(opts), nullptr);
}
#endif

std::unique_ptr<transport::TransportLayer> createAsioTransportLayer(ServiceContext* sc) {
    transport::AsioTransportLayer::Options opts;
    opts.mode = transport::AsioTransportLayer::Options::kEgress;
    return std::make_unique<transport::AsioTransportLayer>(opts, nullptr);
}

ServiceContext::ConstructorActionRegisterer registerEgressTransportLayer{
    "ConfigureEgressTransportLayer",
    [](ServiceContext* sc) {
        invariant(!sc->getTransportLayerManager());

#ifdef MONGO_CONFIG_GRPC
        auto tl = unittest::shouldUseGRPCEgress() ? createGRPCTransportLayer(sc)
                                                  : createAsioTransportLayer(sc);
#else
        auto tl = createAsioTransportLayer(sc);
#endif

        sc->setTransportLayerManager(
            std::make_unique<transport::TransportLayerManagerImpl>(std::move(tl)));
        uassertStatusOK(sc->getTransportLayerManager()->setup());
        uassertStatusOK(sc->getTransportLayerManager()->start());
    },
    [](ServiceContext* sc) {
        auto tlm = sc->getTransportLayerManager();
        if (tlm) {
            tlm->shutdown();
        }
    }};

}  // namespace
}  // namespace mongo
