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
#include "mongo/db/extension/sdk/test_extension_util.h"

namespace sdk = mongo::extension::sdk;
using namespace mongo;

inline constexpr std::string_view kShardedExecutionSerializationStageName =
    "$shardedExecutionSerialization";

class ShardedExecutionExecAggStage : public sdk::ExecAggStage {
public:
    ShardedExecutionExecAggStage() : sdk::ExecAggStage(kShardedExecutionSerializationStageName) {}

    mongo::extension::ExtensionGetNextResult getNext(
        const sdk::QueryExecutionContextHandle& execCtx,
        const MongoExtensionExecAggStage* execStage) override {
        return mongo::extension::ExtensionGetNextResult::pauseExecution();
    }
};

class ShardedExecutionSerializationLogicalStage : public sdk::LogicalAggStage {
public:
    static constexpr StringData kShardedAssertFlagFieldName = "assertFlag";

    ShardedExecutionSerializationLogicalStage() : sdk::LogicalAggStage() {}

    BSONObj serialize() const override {
        return BSON("$shardedExecutionSerialization" << BSON(kShardedAssertFlagFieldName << true));
    }

    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSONObj();
    }

    std::unique_ptr<extension::sdk::ExecAggStage> compile() const override {
        return std::make_unique<ShardedExecutionExecAggStage>();
    }
};

class ShardedExecutionSerializationAstNode : public sdk::AggStageAstNode {
public:
    ShardedExecutionSerializationAstNode()
        : sdk::AggStageAstNode(kShardedExecutionSerializationStageName) {}

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<ShardedExecutionSerializationLogicalStage>();
    }
};

class ShardedExecutionSerializationParseNode : public sdk::AggStageParseNode {
public:
    ShardedExecutionSerializationParseNode()
        : sdk::AggStageParseNode(kShardedExecutionSerializationStageName) {}

    size_t getExpandedSize() const override {
        return 1;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> expanded;
        expanded.reserve(getExpandedSize());
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(
            std::make_unique<ShardedExecutionSerializationAstNode>()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }
};

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
        sdk::validateStageDefinition(stageBson, kStageName);

        sdk_uassert(
            11173701,
            "Intended assertion in sharded scenarios tripped",
            !stageBson.getField(kStageName)
                 .Obj()
                 .hasField(ShardedExecutionSerializationLogicalStage::kShardedAssertFlagFieldName));

        return std::make_unique<ShardedExecutionSerializationParseNode>();
    }
};

class ShardedExecutionSerializationExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<ShardedExecutionSerializationStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(ShardedExecutionSerializationExtension)
DEFINE_GET_EXTENSION()
