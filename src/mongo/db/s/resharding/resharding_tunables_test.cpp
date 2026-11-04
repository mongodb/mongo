// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_tunables.h"

#include "mongo/db/s/resharding/resharding_runtime_server_parameters_gen.h"
#include "mongo/db/server_parameter.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <cctype>
#include <set>
#include <string>
#include <string_view>

namespace mongo {
namespace resharding {
namespace {

TEST(ReshardingTunablesTest, DefaultsComeFromServerParameters) {
    unittest::ServerParameterGuard batchSize{"reshardingCollectionClonerBatchSizeCount", 42};
    unittest::ServerParameterGuard timeout{"reshardingCriticalSectionTimeoutMillis", 1234};

    ReshardingTunables tunables;
    fillDefaults(tunables);

    ASSERT_EQ(tunables.getCollectionClonerBatchSizeCount(), 42);
    ASSERT_EQ(tunables.getCriticalSectionTimeoutMillis(), 1234);
    ASSERT_EQ(tunables.getOplogFetcherInsertBatchLimitBytes(),
              gReshardingOplogFetcherInsertBatchLimitBytes.load());
}

TEST(ReshardingTunablesTest, ExplicitValuesAreNeverOverwritten) {
    unittest::ServerParameterGuard batchSize{"reshardingCollectionClonerBatchSizeCount", 42};
    unittest::ServerParameterGuard timeout{"reshardingCriticalSectionTimeoutMillis", 1234};

    ReshardingTunables tunables;
    tunables.setCollectionClonerBatchSizeCount(7);
    tunables.setCriticalSectionTimeoutMillis(8);
    fillDefaults(tunables);

    ASSERT_EQ(tunables.getCollectionClonerBatchSizeCount(), 7);
    ASSERT_EQ(tunables.getCriticalSectionTimeoutMillis(), 8);
}

/**
 * Maps a resharding server parameter name to the name of the tunable that mirrors it: the
 * "resharding" prefix is dropped (when present) and the first letter is lowercased.
 */
std::string tunableNameFor(const std::string& serverParameterName) {
    constexpr std::string_view kPrefix = "resharding";
    if (!std::string_view{serverParameterName}.starts_with(kPrefix)) {
        return serverParameterName;
    }
    auto name = serverParameterName.substr(kPrefix.size());
    name[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
    return name;
}

TEST(ReshardingTunablesTest, TunablesMatchReshardingServerParameters) {
    std::set<std::string> expected;
    for (const auto& [name, parameter] : ServerParameterSet::getNodeParameterSet()->getMap()) {
        if (dynamic_cast<IDLServerParameterDeprecatedAlias*>(parameter.get())) {
            // A deprecated alias of another server parameter.
            continue;
        }
        const std::string_view sd{name};
        if ((sd.starts_with("resharding") || sd.starts_with("remainingResharding")) &&
            parameter->allowedToChangeAtRuntime()) {
            expected.insert(tunableNameFor(name));
        }
    }
    ASSERT_FALSE(expected.empty());

    ReshardingTunables tunables;
    fillDefaults(tunables);
    const auto actual = tunables.toBSON().getFieldNames<std::set<std::string>>();

    // Every server parameter must have a tunable that is populated by fillDefaults(),
    // and every populated tunable must correspond to a server parameter.
    for (const auto& name : expected) {
        ASSERT_TRUE(actual.contains(name)) << "No populated tunable for server parameter " << name;
    }
    for (const auto& name : actual) {
        ASSERT_TRUE(expected.contains(name)) << "Tunable without a server parameter: " << name;
    }
}

TEST(ReshardingTunablesTest, FilledDefaultsPassIdlValidation) {
    // The IDL validators intentionally duplicate the ones on the server parameters. This guards
    // against a default that the server accepts but the struct would reject.
    ReshardingTunables tunables;
    fillDefaults(tunables);

    auto reparsed = ReshardingTunables::parse(tunables.toBSON(), IDLParserContext{"test"});
    ASSERT_BSONOBJ_EQ(reparsed.toBSON(), tunables.toBSON());
}

}  // namespace
}  // namespace resharding
}  // namespace mongo
