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

#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/grpc/core_test.grpc.pb.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"

#include <memory>
#include <string>
#include <tuple>

#include "cool/import/replacement/core_test_strip_prefix.grpc.pb.h"
#include <absl/status/status.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/status.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport {
namespace test {
namespace {

std::string grpcStatusCodeToString(grpc::StatusCode statusCode) {
    // `absl::StatusCode` enumerates the same values as `grpc::StatusCode`, but `absl::StatusCode`
    // is newer and has a string representation.
    return absl::StatusCodeToString(absl::StatusCode(int(statusCode)));
}

}  // namespace

class GreeterClient {
public:
    GreeterClient(std::shared_ptr<grpc::Channel> channel) : _stub(Greeter::NewStub(channel)) {}

    std::tuple<grpc::Status, std::string> sayHello(const std::string& user) {
        grpc::ClientContext context;
        HelloReply reply;
        HelloRequest request;
        request.set_name(user);
        if (auto status = _stub->sayHello(&context, request, &reply); status.ok()) {
            return {status, reply.message()};
        } else {
            LOGV2_ERROR(7516103,
                        "RPC failed",
                        "code"_attr = status.error_code(),
                        "codeName"_attr = grpcStatusCodeToString(status.error_code()),
                        "message"_attr = status.error_message());
            return {status, ""};
        }
    }

private:
    std::unique_ptr<Greeter::Stub> _stub;
};

std::tuple<grpc::Status, std::string> runClient(std::string serverAddress) {
    LOGV2(7516102, "Client is connecting to the server");
    GreeterClient greeter(grpc::CreateChannel(serverAddress, grpc::InsecureChannelCredentials()));
    return greeter.sayHello("world");
}

class GreeterServiceImpl final : public Greeter::Service {
    grpc::Status sayHello(grpc::ServerContext*,
                          const HelloRequest* request,
                          HelloReply* reply) override {
        reply->set_message("Hello " + request->name());
        return grpc::Status::OK;
    }
};

TEST(GRPCCore, HelloWorld) {
    constexpr auto kServerAddress = "localhost:50051";

    GreeterServiceImpl service;
    grpc::EnableDefaultHealthCheckService(true);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(kServerAddress, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());

    stdx::thread serverThread([&] {
        LOGV2(7516101, "Server is listening for connections", "address"_attr = kServerAddress);
        server->Wait();
    });

    auto pf = makePromiseFuture<std::tuple<grpc::Status, std::string>>();
    stdx::thread clientThread(
        [&] { pf.promise.setWith([&] { return runClient(kServerAddress); }); });
    const auto& [grpcStatus, response] = pf.future.get();
    ASSERT_EQ(grpcStatus.error_code(), grpc::StatusCode::OK);
    ASSERT_EQ(response, "Hello world");

    server->Shutdown();
    clientThread.join();
    serverThread.join();
}

TEST(GRPCCore, HelloWorld2) {
    TestPerson person;
    person.set_name("Joe");
    ASSERT_EQ("Joe", person.name());
}

}  // namespace test
}  // namespace mongo::transport
