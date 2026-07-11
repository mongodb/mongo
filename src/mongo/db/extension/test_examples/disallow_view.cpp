// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/public/extension_agg_stage_static_properties_gen.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"

#include <string_view>
namespace sdk = mongo::extension::sdk;
using namespace mongo;

/**
 * Test extension that disallows views by uasserting in its bindResolvedNamespace implementation.
 * Registers two variants: a transform stage and a source stage.
 */

DEFAULT_EXEC_STAGE(DisallowView)
DEFAULT_LOGICAL_STAGE(DisallowView)

class DisallowViewAstNode : public sdk::AggStageAstNode {
public:
    DisallowViewAstNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::AggStageAstNode(stageName), _arguments(arguments.getOwned()) {}

    void bindResolvedNamespace(const sdk::ResolvedNamespace& resolvedNamespace) override {
        std::string message = str::stream() << "Stage " << std::string(getName())
                                            << " does not support views. Attempted to use in view: "
                                            << std::string(resolvedNamespace.viewName());
        sdk_uasserted(11507700, message);
    }

    std::unique_ptr<sdk::LogicalAggStage> promote(
        const ::MongoExtensionCatalogContext& catalogContext) const override {
        return std::make_unique<DisallowViewLogicalStage>(getName(), _arguments);
    }

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        return std::make_unique<DisallowViewAstNode>(getName(), _arguments);
    }

private:
    const mongo::BSONObj _arguments;
};

DEFAULT_PARSE_NODE(DisallowView)

using DisallowViewStageDescriptor =
    sdk::TestStageDescriptor<"$disallowViews", DisallowViewParseNode, true>;

class DisallowViewSourceExecStage : public sdk::ExecAggStageSource {
public:
    DisallowViewSourceExecStage(std::string_view stageName, const mongo::BSONObj& /*arguments*/)
        : sdk::ExecAggStageSource(stageName) {}

    mongo::extension::ExtensionGetNextResult getNext(
        const mongo::extension::sdk::QueryExecutionContextHandle& /*execCtx*/,
        ::MongoExtensionExecAggStage* /*execStage*/) override {
        return mongo::extension::ExtensionGetNextResult::eof();
    }

    void open() override {}
    void reopen() override {}
    void close() override {}
    mongo::BSONObj explain(const sdk::QueryExecutionContextHandle&,
                           ::MongoExtensionExplainVerbosity /*verbosity*/) const override {
        return mongo::BSONObj();
    }
};

DEFAULT_LOGICAL_STAGE(DisallowViewSource)

class DisallowViewSourceAstNode : public sdk::TestAstNode<DisallowViewSourceLogicalStage> {
public:
    DisallowViewSourceAstNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::TestAstNode<DisallowViewSourceLogicalStage>(stageName, arguments) {}

    mongo::BSONObj getProperties() const override {
        mongo::extension::MongoExtensionStaticProperties properties;
        properties.setPosition(mongo::extension::MongoExtensionPositionRequirementEnum::kFirst);
        properties.setHostType(mongo::extension::MongoExtensionHostTypeRequirementEnum::
                                   kCollectionlessSourceRunOnceAnyNode);
        properties.setRequiresInputDocSource(false);
        properties.setAllowedInFacet(false);

        mongo::BSONObjBuilder builder;
        properties.serialize(&builder);
        return builder.obj();
    }

    void bindResolvedNamespace(const sdk::ResolvedNamespace& resolvedNamespace) override {
        std::string message = str::stream() << "Stage " << std::string(getName())
                                            << " does not support views. Attempted to use in view: "
                                            << std::string(resolvedNamespace.viewName());
        sdk_uasserted(12756000, message);
    }

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        return std::make_unique<DisallowViewSourceAstNode>(getName(), _arguments);
    }
};

DEFAULT_PARSE_NODE(DisallowViewSource)

using DisallowViewSourceStageDescriptor =
    sdk::TestStageDescriptor<"$disallowViewsSource", DisallowViewSourceParseNode, true>;

class DisallowViewExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<DisallowViewStageDescriptor>(portal);
        _registerStage<DisallowViewSourceStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(DisallowViewExtension)
DEFINE_GET_EXTENSION()
