// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/intent_registry_test_fixture.h"

namespace mongo::rss::consensus {
IntentRegistryTest::IntentRegistryTest()
    : _intentRegistry(IntentRegistry::get(getServiceContext())) {}

void IntentRegistryTest::tearDown() {
    // Ensure the registry is enabled before the fixture destructor's GlobalLock(MODE_X),
    // since a disabled registry rejects all intent registrations.
    _intentRegistry.enable();
    ServiceContextMongoDTest::tearDown();
}

bool IntentRegistryTest::containsToken(IntentRegistry::IntentToken token) const {
    auto& tokenMap = _intentRegistry._tokenMaps[(size_t)token.intent()];
    std::lock_guard<std::mutex> lock(tokenMap.lock);
    return tokenMap.map.contains(token.id());
}

size_t IntentRegistryTest::getMapSize(IntentRegistry::Intent intent) const {
    auto& tokenMap = _intentRegistry._tokenMaps[(size_t)intent];
    std::lock_guard<std::mutex> lock(tokenMap.lock);
    return tokenMap.map.size();
}

}  // namespace mongo::rss::consensus
