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

#include "mongo/transport/grpc/mock_client.h"
#include "mongo/transport/grpc/util.h"

namespace mongo::transport::grpc {

MONGO_FAIL_POINT_DEFINE(grpcHangOnStreamEstablishment);

MockClient::MockClient(TransportLayer* tl,
                       ServiceContext* svcCtx,
                       HostAndPort local,
                       MockResolver resolver,
                       const BSONObj& metadata)
    : Client(tl, svcCtx, metadata), _local(std::move(local)), _resolver(std::move(resolver)) {
    _pool = std::make_shared<MockChannelPool>(
        svcCtx->getFastClockSource(),
        [](auto) { return true; },
        [resolver = _resolver, local = _local](const HostAndPort& remote, bool) {
            return std::make_shared<MockChannel>(local, remote, resolver(remote));
        },
        [](std::shared_ptr<MockChannel>& channel, Milliseconds) { return MockStub(channel); });
}

Future<Client::CallContext> MockClient::_streamFactory(const HostAndPort& remote,
                                                       const std::shared_ptr<GRPCReactor>& reactor,
                                                       Milliseconds timeout,
                                                       const ConnectOptions& options,
                                                       const CancellationToken& token) {
    if (MONGO_unlikely(grpcHangOnStreamEstablishment.shouldFail())) {
        // When this failpoint is set, take advantage of the cancellation token to block the future
        // until it has been cancelled, which ensures that we will hit the connect timeout codepath.
        return token.onCancel().unsafeToInlineFuture().onCompletion(
            [](Status s) -> StatusWith<Client::CallContext> {
                return Status(ErrorCodes::CallbackCanceled,
                              "Cancelled stream establishment attempt due to failpoint");
            });
    }

    auto stub = _pool->createStub(remote, options.sslMode, timeout);
    auto ctx = std::make_shared<MockClientContext>();
    setMetadataOnClientContext(*ctx, options);

    token.onCancel().unsafeToInlineFuture().getAsync([ctx, reactor](Status s) {
        if (!s.isOK()) {
            return;
        }
        // Send a best-effort attempt to cancel the ongoing stream establishment.
        reactor->schedule([ctx](Status s) {
            if (!s.isOK()) {
                return;
            }
            ctx->tryCancel();
        });
    });

    std::shared_ptr<ClientStream> stream;
    if (options.authToken) {
        stream = stub->stub().authenticatedCommandStream(ctx.get(), reactor);
    } else {
        stream = stub->stub().unauthenticatedCommandStream(ctx.get(), reactor);
    }

    return Future<CallContext>::makeReady(CallContext{ctx, std::move(stream), {}});
}


}  // namespace mongo::transport::grpc
