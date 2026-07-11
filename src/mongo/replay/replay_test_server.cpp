// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/replay/replay_test_server.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"

namespace mongo {

ReplayTestServer::ReplayTestServer(std::string hostname /*= "$local:12345" */)
    : _hostName(std::move(hostname)) {
    setUp();
}

ReplayTestServer::ReplayTestServer(const std::vector<std::string>& names,
                                   const std::vector<std::string>& responses,
                                   std::string hostName)
    : ReplayTestServer(hostName) {
    invariant(names.size() == responses.size());
    for (size_t i = 0; i < names.size(); ++i) {
        setupServerResponse(names[i], responses[i]);
    }
}

ReplayTestServer::~ReplayTestServer() {
    tearDown();
}

void ReplayTestServer::setupServerResponse(const std::string& name, const std::string& response) {
    tassert(ErrorCodes::ReplayClientInternalError, "mock server is null", _mockServer);
    _fakeResponseMap.insert({name, response});
    _mockServer->setCommandReply(name, fromjson(response));
}

const std::string& ReplayTestServer::getFakeResponse(const std::string& name) const {
    tassert(ErrorCodes::ReplayClientInternalError,
            "fake response was not set correctly",
            _fakeResponseMap.contains(name));
    auto it = _fakeResponseMap.find(name);
    return it->second;
}

std::string ReplayTestServer::getConnectionString() const {
    tassert(ErrorCodes::ReplayClientInternalError, "mock server is null", _mockServer);
    return _mockServer->getServerHostAndPort().toString();
}

void ReplayTestServer::setUp() {
    tassert(ErrorCodes::ReplayClientInternalError, "mock server is not null", !_mockServer);
    auto& settings = logv2::LogManager::global().getGlobalSettings();
    _originalSeverity = settings.getMinimumLogSeverity(logv2::LogComponent::kNetwork).toInt();
    settings.setMinimumLoggedSeverity(logv2::LogComponent::kNetwork, logv2::LogSeverity::Debug(1));

    ConnectionString::setConnectionHook(MockConnRegistry::get()->getConnStrHook());
    _mockServer = std::make_unique<MockRemoteDBServer>(_hostName);
    MockConnRegistry::get()->addServer(_mockServer.get());
}

void ReplayTestServer::tearDown() {
    MockConnRegistry::get()->removeServer(_hostName);
    auto& settings = logv2::LogManager::global().getGlobalSettings();
    settings.setMinimumLoggedSeverity(logv2::LogComponent::kNetwork,
                                      logv2::LogSeverity::cast(_originalSeverity));
}

bool ReplayTestServer::checkResponse(const std::string& name, const BSONObj& response) const {

    tassert(ErrorCodes::ReplayClientInternalError,
            "fake response was not set correctly",
            _fakeResponseMap.contains(name));
    auto it = _fakeResponseMap.find(name);
    const auto& expected = fromjson(it->second);
    bool okFieldNeeded = (bool)expected.getField("ok");
    const auto& actual = okFieldNeeded ? response : response.removeField("ok");
    return expected.toString() == actual.toString();
}

}  // namespace mongo
