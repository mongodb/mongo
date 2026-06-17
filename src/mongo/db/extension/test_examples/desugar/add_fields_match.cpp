/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
