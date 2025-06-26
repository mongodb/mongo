/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/replay/replay_test_server.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"

namespace mongo {

ReplayTestServer::ReplayTestServer() {
    setUp();
}

ReplayTestServer::ReplayTestServer(const std::vector<std::string>& names,
                                   const std::vector<std::string>& responses)
    : ReplayTestServer() {
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
