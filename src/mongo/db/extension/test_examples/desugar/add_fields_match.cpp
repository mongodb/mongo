// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/host_portal.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"

namespace sdk = mongo::extension::sdk;
using namespace mongo;

static const std::string kAddFieldsMatchName = "$addFieldsMatch";
static const std::string kAddFieldsName = "$addFields";
static const std::string kMatchName = "$match";

class AddFieldsMatchParseNode : public sdk::AggStageParseNode {
public:
    // Input: {$addFieldsMatch: {field: "fieldName", value: <expr>, filter: <expr>}}.
    AddFieldsMatchParseNode(BSONObj spec)
        : sdk::AggStageParseNode(kAddFieldsMatchName), _input(spec.getOwned()) {
        // Build {$addFields: {<fieldName>: <value>}}.
        BSONObjBuilder addFieldsInner;
        addFieldsInner.appendAs(_input["value"], _input["field"].str());
        _addFieldsSpec = BSON(kAddFieldsName << addFieldsInner.obj());

        // Build {$match: {$expr: <filter>}}.
        BSONObjBuilder matchInner;
        matchInner.appendAs(_input["filter"], "$expr");
        _matchSpec = BSON(kMatchName << matchInner.obj());
    }

    size_t getExpandedSize() const override {
        return 2;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> out;
        out.reserve(getExpandedSize());

        // Expands to two stages:
        // 1) Host $addFields.
        // 2) Host $match with $expr.
        auto& hostServices = sdk::HostServicesAPI::getInstance();
        out.emplace_back(hostServices->createHostAggStageParseNode(_addFieldsSpec));
        out.emplace_back(hostServices->createHostAggStageParseNode(_matchSpec));

        return out;
    }

    BSONObj getQueryShape(const sdk::QueryShapeOptsHandle&) const override {
        return BSON(kAddFieldsMatchName << _input);
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<AddFieldsMatchParseNode>(_input);
    }

private:
    const BSONObj _input;
    BSONObj _addFieldsSpec;
    BSONObj _matchSpec;
};

class AddFieldsMatchStageDescriptor
    : public sdk::TestStageDescriptor<"$addFieldsMatch", AddFieldsMatchParseNode> {
public:
    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj stageBson) const override {
        const auto obj = sdk::validateStageDefinition(stageBson, kStageName);

        sdk_uassert(10956516,
                    "$addFieldsMatch requires 'field' string",
                    obj.hasField("field") && obj["field"].type() == BSONType::string);
        sdk_uassert(10956517, "$addFieldsMatch requires 'value' field", obj.hasField("value"));
        sdk_uassert(10956518, "$addFieldsMatch requires 'filter' field", obj.hasField("filter"));

        return std::make_unique<AddFieldsMatchParseNode>(obj.getOwned());
    }
};

DEFAULT_EXTENSION(AddFieldsMatch)
REGISTER_EXTENSION(AddFieldsMatchExtension)
DEFINE_GET_EXTENSION()
