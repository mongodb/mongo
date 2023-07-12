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

#include <memory>

#include "mongo/transport/grpc/grpc_transport_layer.h"

#include "mongo/transport/grpc/client.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/ssl_options.h"

namespace mongo::transport::grpc {

GRPCTransportLayer::GRPCTransportLayer(ServiceContext* svcCtx, const WireSpec& wireSpec)
    : TransportLayer(wireSpec), _svcCtx(svcCtx) {}

Status GRPCTransportLayer::start() try {
    invariant(!_client);
    // TODO SERVER-74020: start the Server.
    GRPCClient::Options clientOptions;
    if (!sslGlobalParams.sslCAFile.empty()) {
        clientOptions.tlsCAFile = sslGlobalParams.sslCAFile;
    }
    if (!sslGlobalParams.sslPEMKeyFile.empty()) {
        clientOptions.tlsCertificateKeyFile = sslGlobalParams.sslPEMKeyFile;
    }
    _client = std::make_unique<GRPCClient>(
        this, /* client metadata */ boost::none, std::move(clientOptions));
    _client->start(_svcCtx);
    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

void GRPCTransportLayer::shutdown() {
    // TODO SERVER-74020: shutdown the Server and Client services.
    _client->shutdown();
}

}  // namespace mongo::transport::grpc
