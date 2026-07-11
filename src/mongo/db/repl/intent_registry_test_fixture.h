// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/intent_registry.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/modules.h"

#include <utility>

namespace mongo::rss::consensus {
/**
 * Test fixture for the Intent Registration system.
 */
class IntentRegistryTest : public ServiceContextMongoDTest {
public:
    IntentRegistryTest();

    void tearDown() override;

protected:
    IntentRegistry& _intentRegistry;
    bool containsToken(IntentRegistry::IntentToken token) const;
    size_t getMapSize(IntentRegistry::Intent intent) const;
};
}  // namespace mongo::rss::consensus
