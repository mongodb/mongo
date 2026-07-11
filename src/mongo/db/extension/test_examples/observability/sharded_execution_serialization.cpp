// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

#include <string_view>

namespace sdk = mongo::extension::sdk;
using namespace mongo;

class ShardedExecutionSerializationLogicalStage
    : public sdk::TestLogicalStage<sdk::shared_test_stages::TransformExecAggStage> {
public:
    static constexpr std::string_view kShardedAssertFlagFieldName = "assertFlag";

    ShardedExecutionSerializationLogicalStage(std::string_view stageName, const BSONObj& spec)
        : sdk::TestLogicalStage<sdk::shared_test_stages::TransformExecAggStage>(stageName, spec) {}

    BSONObj serialize() const override {
        return BSON(_name << BSON(kShardedAssertFlagFieldName << true));
    }

    std::unique_ptr<extension::sdk::LogicalAggStage> clone() const override {
        return std::make_unique<ShardedExecutionSerializationLogicalStage>(_name, _arguments);
    }
};

DEFAULT_AST_NODE(ShardedExecutionSerialization);
DEFAULT_PARSE_NODE(ShardedExecutionSerialization);

/**
 * A test-only extension that serializes itself on mongos in such a way that when sent to mongod,
 * parsing the stage trips a uassert. If the stage's spec contains the field
 * `ShardedExecutionSerializationLogicalStage::kShardedAssertFlagFieldName`, parsing will uassert
 * with a special code.
 *
 * This special code can be used to verify that the `serialize()` code-path in the `LogicalAggStage`
 * is actually being called.
 */
class ShardedExecutionSerializationStageDescriptor
    : public sdk::TestStageDescriptor<"$shardedExecutionSerialization",
                                      ShardedExecutionSerializationParseNode> {
public:
    void validate(const BSONObj& arguments) const override {
        sdk_uassert(11173701,
                    "Intended assertion in sharded scenarios tripped",
                    !arguments.hasField(
                        ShardedExecutionSerializationLogicalStage::kShardedAssertFlagFieldName));
    }
};

DEFAULT_EXTENSION(ShardedExecutionSerialization)
REGISTER_EXTENSION(ShardedExecutionSerializationExtension)
DEFINE_GET_EXTENSION()
