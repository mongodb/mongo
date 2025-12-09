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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

namespace sdk = mongo::extension::sdk;
using namespace mongo;

class ShardedExecutionSerializationLogicalStage
    : public sdk::TestLogicalStage<sdk::shared_test_stages::TransformExecAggStage> {
public:
    static constexpr StringData kShardedAssertFlagFieldName = "assertFlag";

    ShardedExecutionSerializationLogicalStage(std::string_view stageName, const BSONObj& spec)
        : sdk::TestLogicalStage<sdk::shared_test_stages::TransformExecAggStage>(stageName, spec) {}

    BSONObj serialize() const override {
        return BSON(_name << BSON(kShardedAssertFlagFieldName << true));
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
class ShardedExecutionSerializationStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = "$shardedExecutionSerialization";

    ShardedExecutionSerializationStageDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        auto arguments = sdk::validateStageDefinition(stageBson, kStageName);

        sdk_uassert(11173701,
                    "Intended assertion in sharded scenarios tripped",
                    !arguments.hasField(
                        ShardedExecutionSerializationLogicalStage::kShardedAssertFlagFieldName));

        return std::make_unique<ShardedExecutionSerializationParseNode>(kStageName, arguments);
    }
};

DEFAULT_EXTENSION(ShardedExecutionSerialization)
REGISTER_EXTENSION(ShardedExecutionSerializationExtension)
DEFINE_GET_EXTENSION()
