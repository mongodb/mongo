// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

namespace sdk = mongo::extension::sdk;

namespace {
constexpr char kInternalStageName[] = "$_testInternalStage";

/**
 * $_testInternalStage is an internal-only transform stage: it reuses the shared transform chain but
 * declares itself internal-only, so a user pipeline using it is rejected with error 5491300.
 */
class InternalStageDescriptor
    : public sdk::TestStageDescriptor<kInternalStageName,
                                      sdk::shared_test_stages::TransformAggStageParseNode> {
public:
    ::MongoExtensionClientType getClientType() const override {
        return ::kMongoExtensionClientTypeInternal;
    }
};

constexpr char kDesugarsToInternalName[] = "$testDesugarsToInternal";

/**
 * $testDesugarsToInternal is a user-facing stage that desugars into a transform stage followed by
 * the internal-only $_testInternalStage, mirroring the $search -> $_internalMongotRemote shape.
 * Internal-only enforcement applies to what the user wrote, so this stage must remain usable from
 * user pipelines even though its expansion contains an internal-only stage.
 */
class DesugarsToInternalParseNode : public sdk::AggStageParseNode {
public:
    DesugarsToInternalParseNode() : sdk::AggStageParseNode(kDesugarsToInternalName) {}

    size_t getExpandedSize() const override {
        return 2;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> expanded;
        expanded.reserve(getExpandedSize());
        expanded.emplace_back(
            mongo::extension::AggStageAstNodeHandle{new sdk::ExtensionAggStageAstNodeAdapter(
                std::make_unique<sdk::shared_test_stages::TransformAggStageAstNode>(
                    kDesugarsToInternalName, mongo::BSONObj()))});
        expanded.emplace_back(
            mongo::extension::AggStageAstNodeHandle{new sdk::ExtensionAggStageAstNodeAdapter(
                std::make_unique<sdk::shared_test_stages::TransformAggStageAstNode>(
                    kInternalStageName, mongo::BSONObj()))});
        return expanded;
    }

    mongo::BSONObj getQueryShape(const sdk::QueryShapeOptsHandle& ctx) const override {
        return BSON(std::string(getName()) << mongo::BSONObj());
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<DesugarsToInternalParseNode>();
    }
};

using DesugarsToInternalDescriptor =
    sdk::TestStageDescriptor<kDesugarsToInternalName, DesugarsToInternalParseNode>;

class InternalStageExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<InternalStageDescriptor>(portal);
        _registerStage<DesugarsToInternalDescriptor>(portal);
    }
};
}  // namespace

REGISTER_EXTENSION(InternalStageExtension)
DEFINE_GET_EXTENSION()
