// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/grpc/mock_client.h"

#include "mongo/transport/grpc/util.h"

namespace mongo::transport::grpc {

MONGO_FAIL_POINT_DEFINE(grpcHangOnChannelEstablishment);
MONGO_FAIL_POINT_DEFINE(grpcFailChannelEstablishment);

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
        [](std::shared_ptr<MockChannel>& channel) { return MockStub(channel); });
}

Future<Client::CallContext> MockClient::_streamFactory(const HostAndPort& remote,
                                                       const std::shared_ptr<GRPCReactor>& reactor,
                                                       boost::optional<Date_t> deadline,
                                                       const ConnectOptions& options,
                                                       const CancellationToken& token) {
    if (MONGO_unlikely(grpcHangOnChannelEstablishment.shouldFail())) {
        // When this failpoint is set, take advantage of the cancellation token to block the future
        // until it has been cancelled, which ensures that we will hit the connect timeout codepath.
        return token.onCancel().unsafeToInlineFuture().onCompletion(
            [](Status s) -> StatusWith<Client::CallContext> {
                return Status(ErrorCodes::CallbackCanceled,
                              "hanging channel establishment attempt due to failpoint");
            });
    } else if (MONGO_unlikely(grpcFailChannelEstablishment.shouldFail())) {
        return Future<Client::CallContext>::makeReady(
            Status(grpc::util::statusToErrorCode(::grpc::StatusCode::UNAVAILABLE),
                   "failing channel establishment due to fail point"));
    }
    auto stub = _pool->createStub(remote, options.sslMode);
    auto ctx = std::make_shared<MockClientContext>();
    setMetadataOnClientContext(*ctx, options);

    std::shared_ptr<ClientStream> stream;
    if (options.authToken) {
        stream = stub->stub().authenticatedCommandStream(ctx.get(), reactor);
    } else {
        stream = stub->stub().unauthenticatedCommandStream(ctx.get(), reactor);
    }

    return Future<CallContext>::makeReady(CallContext{ctx, std::move(stream), {}, UUID::gen()});
}


}  // namespace mongo::transport::grpc
