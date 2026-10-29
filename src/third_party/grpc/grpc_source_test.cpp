// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0



#include <cstdarg>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "mongo/unittest/unittest.h"
#include "greeter_server.h"

using namespace mongo;

TEST(GrpcLibraryIntegration, StartServer) {
    std::string server_address("127.0.0.1:50051");
    GreeterServiceImpl service;

    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    ServerBuilder builder;
    // Listen on the given address without any authentication mechanism.
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    // Register "service" as the instance through which we'll communicate with
    // clients. In this case it corresponds to an *synchronous* service.
    builder.RegisterService(&service);
    // Finally assemble the server.
    std::unique_ptr<Server> server(builder.BuildAndStart());
    if (server == nullptr)
        FAIL("Failed to start GRPC server.");
}
