/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/transport/grpc/grpc_transport_layer_mock.h"

#include "mongo/base/error_codes.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/duration.h"

namespace mongo::transport::grpc {

GRPCTransportLayerMock::GRPCTransportLayerMock(ServiceContext* svcCtx,
                                               GRPCTransportLayer::Options options,
                                               MockClient::MockResolver resolver,
                                               const HostAndPort& mockClientAddress)
    : _svcCtx{std::move(svcCtx)},
      _options{std::move(options)},
      _resolver{std::move(resolver)},
      _mockClientAddress{std::move(mockClientAddress)} {}

Status GRPCTransportLayerMock::registerService(std::unique_ptr<Service> svc) {
    return Status::OK();
}

Status GRPCTransportLayerMock::setup() {
    _client = std::make_shared<MockClient>(
        this, std::move(_mockClientAddress), std::move(_resolver), makeClientMetadataDocument());
    return Status::OK();
}

Status GRPCTransportLayerMock::start() {
    invariant(_client, "Must call setup() before start().");
    _client->start(_svcCtx);
    return Status::OK();
}

void GRPCTransportLayerMock::shutdown() {
    invariant(_client, "Must call setup() before shutdown().");
    _client->shutdown();
}

StatusWith<std::shared_ptr<Session>> GRPCTransportLayerMock::connectWithAuthToken(
    HostAndPort peer, Milliseconds timeout, boost::optional<std::string> authToken) {
    invariant(_client, "Must call setup() before connect().");
    return _client->connect(std::move(peer), std::move(timeout), {std::move(authToken)});
}

StatusWith<std::shared_ptr<Session>> GRPCTransportLayerMock::connect(
    HostAndPort peer,
    ConnectSSLMode sslMode,
    Milliseconds timeout,
    boost::optional<TransientSSLParams> transientSSLParams) {
    return connectWithAuthToken(std::move(peer), std::move(timeout));
}

}  // namespace mongo::transport::grpc
