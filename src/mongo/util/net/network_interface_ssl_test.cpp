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


#include "mongo/client/authenticate.h"
#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_integration_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/ssl_options.h"

#include <fstream>
#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace executor {
namespace {

std::string loadFile(const std::string& name) {
    std::ifstream input(name);
    std::string str((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return str;
}

class NetworkInterfaceSSLFixture : public NetworkInterfaceIntegrationFixture {
public:
    void setUp() final {
        resetIsInternalClient(true);
        NetworkInterfaceIntegrationFixture::setUp();

        // Setup an internal user so that we can use it for external auth
        std::unique_ptr<UserRequest> systemLocal =
            std::make_unique<UserRequestGeneral>(UserName("__system"_sd, "local"_sd), boost::none);
        auto user = std::make_shared<UserHandle>(User(std::move(systemLocal)));

        internalSecurity.setUser(user);

        sslGlobalParams.sslCAFile = "jstests/libs/ca.pem";
        // Set a client cert that should be ignored if we use the transient cert correctly.
        sslGlobalParams.sslPEMKeyFile = "jstests/libs/client.pem";

        // Set the internal user auth parameters so we auth with X.509 externally
        auth::setInternalUserAuthParams(
            auth::createInternalX509AuthDocument(boost::optional<StringData>("Ignored")));

        createNet();
        net().startup();
    }

    std::unique_ptr<NetworkInterface> _makeNet(std::string instanceName,
                                               transport::TransportProtocol protocol) override {
        LOGV2(5181101, "Initializing the test connection with transient SSL params");
        ConnectionPool::Options options = makeDefaultConnectionPoolOptions();
        options.transientSSLParams.emplace([] {
            ClusterConnection clusterConnection;
            clusterConnection.targetedClusterConnectionString = ConnectionString::forLocal();
            clusterConnection.sslClusterPEMPayload = loadFile("jstests/libs/server.pem");

            TransientSSLParams params(clusterConnection);
            return params;
        }());
        return makeNetworkInterface(instanceName, nullptr, nullptr, std::move(options));
    }

    void tearDown() override {
        NetworkInterfaceIntegrationFixture::tearDown();
        resetIsInternalClient(false);
    }
};

TEST_F(NetworkInterfaceSSLFixture, Ping) {
    assertCommandOK(DatabaseName::kAdmin,
                    BSON("ping" << 1),
                    RemoteCommandRequest::kNoTimeout,
                    transport::kEnableSSL);
}

}  // namespace
}  // namespace executor
}  // namespace mongo
