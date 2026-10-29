// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

#include <string_view>

namespace sdk = mongo::extension::sdk;

using BarExecStage = sdk::shared_test_stages::TransformExecAggStage;

/**
 * $testBar reports its arguments as a filter for shard targeting.
 */
class BarLogicalStage : public sdk::TestLogicalStage<BarExecStage> {
public:
    BarLogicalStage(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::TestLogicalStage<BarExecStage>(stageName, arguments) {}

    std::unique_ptr<sdk::LogicalAggStage> clone() const override {
        return std::make_unique<BarLogicalStage>(_name, _arguments);
    }

    mongo::BSONObj getFilter() const override {
        return _arguments;
    }
};

DEFAULT_AST_NODE(Bar)
DEFAULT_PARSE_NODE(Bar)

/**
 * $testBar is a no-op stage.
 *
 * The stage definition must NOT be empty or it will fail to parse. The contents of the stage
 * definition can be anything, as long as it is not an empty object.
 */
class BarStageDescriptor : public sdk::TestStageDescriptor<"$testBar", BarParseNode> {
public:
    void validate(const mongo::BSONObj& arguments) const override {
        sdk_uassert(10785800,
                    "Failed to parse " + kStageName + ", must have at least one field",
                    !arguments.isEmpty());
    }
};

DEFAULT_EXTENSION(Bar)
REGISTER_EXTENSION(BarExtension)
DEFINE_GET_EXTENSION()
