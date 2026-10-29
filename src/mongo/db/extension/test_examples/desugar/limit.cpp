// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"
#include "mongo/db/extension/shared/get_next_result.h"

#include <string_view>

class LimitExecStage : public sdk::TestExecStage {
public:
    LimitExecStage(std::string_view stageName, const mongo::BSONObj& stageBson)
        : sdk::TestExecStage(stageName, stageBson),
          _nReturned(0),
          _limit(stageBson.getOwned()
                     .getField(stageName)
                     .parseIntegerElementToNonNegativeLong()
                     .getValue()) {}

    mongo::extension::ExtensionGetNextResult getNext(
        const mongo::extension::sdk::QueryExecutionContextHandle& execCtx,
        ::MongoExtensionExecAggStage* execStage) override {
        if (_nReturned >= _limit) {
            return mongo::extension::ExtensionGetNextResult::eof();
        }

        auto nextInput = _getSource()->getNext(execCtx.get());
        if (nextInput.code == mongo::extension::GetNextCode::kAdvanced) {
            ++_nReturned;
        }

        return nextInput;
    }

private:
    long long _nReturned;
    const long long _limit;
};

class LimitLogicalStage : public sdk::TestLogicalStage<LimitExecStage> {
public:
    // stageBson example: {$extensionLimit: 3}
    LimitLogicalStage(std::string_view stageName, const mongo::BSONObj& stageBson)
        : sdk::TestLogicalStage<LimitExecStage>(stageName, stageBson),
          _stageBson(stageBson.getOwned()) {}

    mongo::BSONObj serialize() const override {
        // Need to override otherwise default wraps twice: {$extensionLimit: {$extensionLimit: 3}}.
        return _stageBson;
    }

    mongo::BSONObj explain(const sdk::QueryExecutionContextHandle&,
                           ::MongoExtensionExplainVerbosity verbosity) const override {
        // Need to override otherwise default wraps twice: {$extensionLimit: {$extensionLimit: 3}}.
        return _stageBson;
    }

    mongo::BSONObj getDocsNeededBounds() const override {
        auto limit = _stageBson.firstElement().parseIntegerElementToNonNegativeLong().getValue();
        return BSON("effect" << "limit"
                             << "value" << limit);
    }

    std::unique_ptr<sdk::LogicalAggStage> clone() const override {
        return std::make_unique<LimitLogicalStage>(_name, _stageBson);
    }

    boost::optional<sdk::DistributedPlanLogic> getDistributedPlanLogic() const override {
        sdk::DistributedPlanLogic dpl;
        // This stage must run on the merging node and shards (optimization).
        {
            std::vector<mongo::extension::VariantDPLHandle> pipeline;
            pipeline.emplace_back(mongo::extension::LogicalAggStageHandle{
                new mongo::extension::sdk::ExtensionLogicalAggStageAdapter(clone())});
            dpl.shardsPipeline = sdk::DPLArrayContainer(std::move(pipeline));
        }
        {
            std::vector<mongo::extension::VariantDPLHandle> pipeline;
            pipeline.emplace_back(mongo::extension::LogicalAggStageHandle{
                new mongo::extension::sdk::ExtensionLogicalAggStageAdapter(clone())});
            dpl.mergingPipeline = sdk::DPLArrayContainer(std::move(pipeline));
        }
        return dpl;
    }

protected:
    const mongo::BSONObj _stageBson;
};

DEFAULT_AST_NODE(Limit);

class LimitParseNode : public sdk::TestParseNode<LimitAstNode> {
public:
    // stageBson example: {$extensionLimit: 3}
    LimitParseNode(std::string_view stageName, const mongo::BSONObj& stageBson)
        : sdk::TestParseNode<LimitAstNode>(stageName, stageBson), _stageBson(stageBson.getOwned()) {
        // Copied DocumentSourceLimit::createFromBson(...) logic:
        const auto limit = _stageBson.firstElement().parseIntegerElementToNonNegativeLong();
        sdk_uassert(11484701,
                    "invalid argument to $extensionLimit stage: " + limit.getStatus().reason(),
                    limit.isOK());

        // Copied DocumentSourceLimit::create(...) logic:
        sdk_uassert(11484702, "the limit must be a positive number  ", limit.getValue() > 0);
    }

    mongo::BSONObj getQueryShape(const sdk::QueryShapeOptsHandle&) const override {
        // Need to override otherwise default wraps twice: {$extensionLimit: {$extensionLimit: 3}}.
        return _stageBson;
    }

    mongo::BSONObj toBsonForLog() const override {
        return _stageBson;
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<LimitParseNode>(getName(), _stageBson);
    }

protected:
    const mongo::BSONObj _stageBson;
};

/**
 * $extensionLimit is an agg stage that requires a numerical limit value, like {$extensionLimit: 3}.
 */
class LimitStageDescriptor : public sdk::TestStageDescriptor<"$extensionLimit", LimitParseNode> {
public:
    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        // Note that the entire stage BSON (represented as a BSONObj) is given which means that the
        // parse logic must extract the BSONElement associated with the extension stage's stageName
        // first.
        sdk_uassert(11484700, "Failed to parse " + kStageName, stageBson.hasField(kStageName));
        return std::make_unique<LimitParseNode>(kStageName, stageBson);
    }
};

class LimitExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        // Always register $extensionLimit.
        _registerStage<LimitStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(LimitExtension)
DEFINE_GET_EXTENSION()
