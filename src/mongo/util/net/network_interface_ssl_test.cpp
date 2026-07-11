// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/client/authenticate.h"
#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_integration_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/ssl_options.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace executor {
namespace {
using namespace std::literals::string_view_literals;

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
            std::make_unique<UserRequestGeneral>(UserName("__system"sv, "local"sv), boost::none);
        auto user = std::make_shared<UserHandle>(User(std::move(systemLocal)));

        internalSecurity.setUser(user);

        installDir = std::getenv("INSTALL_DIR");

        sslGlobalParams.sslCAFile = (installDir / "x509/ca.pem").string();
        // Set a client cert that should be ignored if we use the transient cert correctly.
        sslGlobalParams.sslPEMKeyFile = (installDir / "x509/client.pem").string();

        // Set the internal user auth parameters so we auth with X.509 externally
        auth::setInternalUserAuthParams(
            auth::createInternalX509AuthCredential(boost::optional<std::string_view>("Ignored")));

        createNet();
        net().startup();
    }

    std::unique_ptr<NetworkInterface> _makeNet(std::string instanceName,
                                               transport::TransportProtocol protocol) override {
        LOGV2(5181101, "Initializing the test connection with transient SSL params");
        ConnectionPool::Options options = makeDefaultConnectionPoolOptions();
        options.transientSSLParams.emplace([this]() {
            ClusterConnection clusterConnection;
            clusterConnection.targetedClusterConnectionString = ConnectionString::forLocal();
            clusterConnection.sslClusterPEMPayload =
                loadFile((installDir / "x509/server.pem").string());

            TransientSSLParams params(clusterConnection);
            return params;
        }());
        return makeNetworkInterface(instanceName, nullptr, nullptr, std::move(options));
    }

    void tearDown() override {
        NetworkInterfaceIntegrationFixture::tearDown();
        resetIsInternalClient(false);
    }

private:
    std::filesystem::path installDir;
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
