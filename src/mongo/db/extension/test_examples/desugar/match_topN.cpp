// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/host_portal.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"

namespace sdk = mongo::extension::sdk;
using namespace mongo;

static const std::string kMatchTopNName = "$matchTopN";
static const std::string kMatchName = "$match";
static const std::string kSortName = "$sort";
static const std::string kLimitName = "$limit";

class MatchTopNParseNode : public sdk::AggStageParseNode {
public:
    // Input: {$matchTopN: {filter: {...}, sort: {...}, limit: <num>}}
    MatchTopNParseNode(BSONObj topN)
        : sdk::AggStageParseNode(kMatchTopNName),
          _input(topN.getOwned()),
          _matchSpec(BSON(kMatchName << _input["filter"].Obj())),
          _sortSpec(BSON(kSortName << _input["sort"].Obj())),
          _limitSpec(BSON(kLimitName << _input["limit"].numberInt())) {}

    size_t getExpandedSize() const override {
        return 3;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> out;
        out.reserve(getExpandedSize());

        // Expands to three stages:
        // 1) Host $match
        // 2) Host $sort
        // 3) Host $limit (TODO SERVER-115424 make this an extension $limit if $extensionLimit
        // deterministically returns the same results as native $limit for sharded collections.
        // Currently, $extensionLimit does not.)
        auto& hostServices = sdk::HostServicesAPI::getInstance();
        out.emplace_back(hostServices->createHostAggStageParseNode(_matchSpec));
        out.emplace_back(hostServices->createHostAggStageParseNode(_sortSpec));
        out.emplace_back(hostServices->createHostAggStageParseNode(_limitSpec));

        return out;
    }

    BSONObj getQueryShape(const sdk::QueryShapeOptsHandle&) const override {
        return BSON(kMatchTopNName << _input);
    }

    BSONObj toBsonForLog() const override {
        return BSON(kMatchTopNName << _input);
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<MatchTopNParseNode>(_input);
    }

private:
    const BSONObj _input;      // {filter: {...}, sort: {...}, limit: <num>}
    const BSONObj _matchSpec;  // {$match: {...}}
    const BSONObj _sortSpec;   // {$sort: {...}}
    const BSONObj _limitSpec;  // {$limit: <int>}
};

class MatchTopNStageDescriptor : public sdk::TestStageDescriptor<"$matchTopN", MatchTopNParseNode> {
public:
    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj stageBson) const override {
        const auto obj = sdk::validateStageDefinition(stageBson, kStageName);

        sdk_uassert(10956500,
                    "$matchTopN requires 'filter' object",
                    obj.hasField("filter") && obj["filter"].isABSONObj());
        sdk_uassert(10956501,
                    "$matchTopN requires 'sort' object",
                    obj.hasField("sort") && obj["sort"].isABSONObj());

        const auto limitElem = obj["limit"];
        sdk_uassert(10956502, "$matchTopN requires a 'limit' field", !limitElem.eoo());
        const auto swLimit = limitElem.parseIntegerElementToNonNegativeLong();
        sdk_uassert(10956515,
                    "$matchTopN requires a positive integral 'limit'",
                    swLimit.isOK() && swLimit.getValue() >= 1);

        return std::make_unique<MatchTopNParseNode>(obj.getOwned());
    }
};

DEFAULT_EXTENSION(MatchTopN)
REGISTER_EXTENSION(MatchTopNExtension)
DEFINE_GET_EXTENSION()
