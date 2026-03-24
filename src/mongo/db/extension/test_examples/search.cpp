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
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

namespace sdk = mongo::extension::sdk;

/**
 * Parse node for $search that desugars into $_extensionSearch.
 */
class SearchParseNode : public sdk::AggStageParseNode {
public:
    SearchParseNode() : sdk::AggStageParseNode("$search") {}

    SearchParseNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::AggStageParseNode(stageName), _arguments(arguments.getOwned()) {}

    size_t getExpandedSize() const override {
        return 1;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> expanded;
        expanded.reserve(1);
        expanded.emplace_back(new sdk::ExtensionAggStageAstNodeAdapter(
            std::make_unique<sdk::shared_test_stages::TransformAggStageAstNode>("$_extensionSearch",
                                                                                _arguments)));
        return expanded;
    }

    mongo::BSONObj getQueryShape(const sdk::QueryShapeOptsHandle&) const override {
        return mongo::BSONObj();
    }

    mongo::BSONObj toBsonForLog() const override {
        return BSON(_name << _arguments);
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<SearchParseNode>(getName(), _arguments);
    }

private:
    mongo::BSONObj _arguments;
};

/**
 * $search is a stage used to imitate overriding the existing $search implementation with an
 * extension stage. It desugars into $_extensionSearch.
 */
using SearchStageDescriptor = sdk::TestStageDescriptor<"$search", SearchParseNode>;

/**
 * Stage descriptor for $_extensionSearch. Even though users don't use $_extensionSearch directly,
 * we must register this stage descriptor for the sharded case, where mongos serializes the
 * pipeline and sends it to the shards, and when merging explain output from shards.
 */
using ExtensionSearchStageDescriptor =
    sdk::TestStageDescriptor<"$_extensionSearch",
                             sdk::shared_test_stages::TransformAggStageParseNode>;

/**
 * Parse node for $searchMeta that desugars into $_extensionSearchMeta.
 */
class SearchMetaParseNode : public sdk::AggStageParseNode {
public:
    SearchMetaParseNode() : sdk::AggStageParseNode("$searchMeta") {}

    SearchMetaParseNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::AggStageParseNode(stageName), _arguments(arguments.getOwned()) {}

    size_t getExpandedSize() const override {
        return 1;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> expanded;
        expanded.reserve(1);
        expanded.emplace_back(new sdk::ExtensionAggStageAstNodeAdapter(
            std::make_unique<sdk::shared_test_stages::TransformAggStageAstNode>(
                "$_extensionSearchMeta", _arguments)));
        return expanded;
    }

    mongo::BSONObj getQueryShape(const sdk::QueryShapeOptsHandle&) const override {
        return mongo::BSONObj();
    }

    mongo::BSONObj toBsonForLog() const override {
        return BSON(_name << _arguments);
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<SearchMetaParseNode>(getName(), _arguments);
    }

private:
    mongo::BSONObj _arguments;
};

/**
 * $searchMeta is a stage used to imitate overriding the existing $searchMeta implementation with
 * an extension stage. It desugars into $_extensionSearchMeta.
 */
using SearchMetaStageDescriptor = sdk::TestStageDescriptor<"$searchMeta", SearchMetaParseNode>;

/**
 * Stage descriptor for $_extensionSearchMeta. Registered for the sharded case where mongos
 * serializes the pipeline and sends it to shards.
 */
using ExtensionSearchMetaStageDescriptor =
    sdk::TestStageDescriptor<"$_extensionSearchMeta",
                             sdk::shared_test_stages::TransformAggStageParseNode>;

class SearchExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<SearchStageDescriptor>(portal);
        _registerStage<ExtensionSearchStageDescriptor>(portal);
        _registerStage<SearchMetaStageDescriptor>(portal);
        _registerStage<ExtensionSearchMetaStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(SearchExtension)
DEFINE_GET_EXTENSION()
