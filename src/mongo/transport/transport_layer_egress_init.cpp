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
