// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/service_context_d_test_fixture.h"

#include <string_view>

namespace mongo {

class DeprecatedServerParameterTest : public ServiceContextMongoDTest {
public:
    void testParameterIsDeprecated(std::string_view parameterName) {
        auto&& m = ServerParameterSet::getNodeParameterSet()->getMap();
        auto it = m.find(parameterName);
        ASSERT(it != m.end()) << "Not found: " << parameterName;
        auto&& [key, sp] = *it;
        ASSERT_EQ(key, parameterName);
        ASSERT_EQ(sp->name(), parameterName);
        ASSERT_TRUE(sp->getIsDeprecated());
    }

    template <typename ValType>
    void testSetParameterWarnsOnce(std::string_view parameterName, ValType value) {
        unittest::LogCaptureGuard logs;
        auto opCtx = cc().makeOperationContext();
        DBDirectClient client(opCtx.get());
        BSONObj result;
        auto cmdBSON = BSON("setParameter" << 1 << parameterName << value);
        ASSERT(client.runCommand(DatabaseName::kAdmin, cmdBSON, result));

        const auto parameterAttrBSON = BSON("attr" << BSON("parameter" << parameterName));

        // Check that the deprecation warning was logged once.
        ASSERT_EQ(logs.countBSONContainingSubset(parameterAttrBSON), 1);

        ASSERT(client.runCommand(DatabaseName::kAdmin, cmdBSON, result));

        // Check that the deprecation warning is not logged again by a repeated setParameter.
        ASSERT_EQ(logs.countBSONContainingSubset(parameterAttrBSON), 1);
    }
};

}  // namespace mongo
