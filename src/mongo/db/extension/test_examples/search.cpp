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
#include "mongo/db/extension/sdk/host_services.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"

namespace sdk = mongo::extension::sdk;

/**
 * Exec stage for $_extensionSearch which returns EOF immediately. Parse-time coverage (DRM +
 * $_extensionSearch + metadata spec) lives in SearchParseNode::expand().
 */
class ExtensionSearchExecStage : public sdk::ExecAggStageResultsAndMetadataSource {
public:
    ExtensionSearchExecStage(std::string_view name, const mongo::BSONObj& /*arguments*/)
        : sdk::ExecAggStageResultsAndMetadataSource(name) {}

    mongo::extension::ExtensionGetNextResult getNext(
        const sdk::QueryExecutionContextHandle& /*execCtx*/,
        ::MongoExtensionExecAggStage* /*execStage*/) override {
        return mongo::extension::ExtensionGetNextResult::eof();
    }

    void open() override {}
    void reopen() override {}
    void close() override {}

    mongo::BSONObj explain(const sdk::QueryExecutionContextHandle&,
                           ::MongoExtensionExplainVerbosity) const override {
        return mongo::BSONObj();
    }
};

DEFAULT_LOGICAL_STAGE(ExtensionSearch)

/**
 * Reports source-stage properties so that DRM accepts it as its 'source'.
 */
class ExtensionSearchAstNode : public sdk::TestAstNode<ExtensionSearchLogicalStage> {
public:
    ExtensionSearchAstNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::TestAstNode<ExtensionSearchLogicalStage>(stageName, arguments) {}

    mongo::BSONObj getProperties() const override {
        mongo::extension::MongoExtensionStaticProperties properties;
        mongo::BSONObjBuilder builder;
        properties.setRequiresInputDocSource(false);
        properties.setPosition(mongo::extension::MongoExtensionPositionRequirementEnum::kFirst);
        properties.setHostType(mongo::extension::MongoExtensionHostTypeRequirementEnum::kAnyShard);
        properties.serialize(&builder);
        return builder.obj();
    }

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        return std::make_unique<ExtensionSearchAstNode>(getName(), _arguments);
    }
};

class SearchParseNodeBase : public sdk::AggStageParseNode {
public:
    SearchParseNodeBase(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::AggStageParseNode(stageName), _arguments(arguments.getOwned()) {}

    size_t getExpandedSize() const override {
        return 1;
    }

    mongo::BSONObj getQueryShape(const sdk::QueryShapeOptsHandle&) const override {
        return mongo::BSONObj();
    }

    mongo::BSONObj toBsonForLog() const override {
        return BSON(_name << _arguments);
    }

protected:
    mongo::BSONObj _arguments;
};

/**
 * Parse node for $search. Desugars into:
 *   [$_internalDocumentResultsAndMetadata(source: $_extensionSearch, metadata: SEARCH_META)]
 */
class SearchParseNode : public SearchParseNodeBase {
public:
    SearchParseNode() : SearchParseNodeBase("$search", {}) {}
    SearchParseNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : SearchParseNodeBase(stageName, arguments) {}

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<SearchParseNode>(getName(), _arguments);
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        const auto& host = sdk::HostServicesAPI::getInstance();
        mongo::BSONObj drmSpec =
            BSON("$_internalDocumentResultsAndMetadata"
                 << BSON("source" << BSON("$_extensionSearch" << _arguments) << "metadata"
                                  << BSON("as" << "SEARCH_META")));
        std::vector<mongo::extension::VariantNodeHandle> expanded;
        expanded.reserve(1);
        expanded.emplace_back(host->createDocumentResultsAndMetadata(drmSpec));
        return expanded;
    }
};

using SearchStageDescriptor = sdk::TestStageDescriptor<"$search", SearchParseNode>;

DEFAULT_PARSE_NODE(ExtensionSearch)

using ExtensionSearchStageDescriptor =
    sdk::TestStageDescriptor<"$_extensionSearch", ExtensionSearchParseNode>;

/**
 * Exec stage for $_extensionSearchMeta: a standalone source stage that returns one metadata
 * document to represent the search metadata result.
 */
class ExtensionSearchMetaExecStage : public sdk::ExecAggStageSource {
public:
    ExtensionSearchMetaExecStage(std::string_view name, const mongo::BSONObj& /*arguments*/)
        : sdk::ExecAggStageSource(name) {}

    mongo::extension::ExtensionGetNextResult getNext(
        const sdk::QueryExecutionContextHandle& /*execCtx*/,
        ::MongoExtensionExecAggStage* /*execStage*/) override {
        if (_emitted) {
            return mongo::extension::ExtensionGetNextResult::eof();
        }
        _emitted = true;
        return mongo::extension::ExtensionGetNextResult::advanced(
            mongo::extension::ExtensionBSONObj::makeAsByteBuf(
                BSON("count" << BSON("lowerBound" << 0))));
    }

    void open() override {}
    void reopen() override {}
    void close() override {}

    mongo::BSONObj explain(const sdk::QueryExecutionContextHandle&,
                           ::MongoExtensionExplainVerbosity) const override {
        return mongo::BSONObj();
    }

private:
    bool _emitted = false;
};

DEFAULT_LOGICAL_STAGE(ExtensionSearchMeta)

/**
 * Reports source-stage properties so that $searchMeta is treated as a kFirst source stage.
 */
class ExtensionSearchMetaAstNode : public sdk::TestAstNode<ExtensionSearchMetaLogicalStage> {
public:
    ExtensionSearchMetaAstNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::TestAstNode<ExtensionSearchMetaLogicalStage>(stageName, arguments) {}

    mongo::BSONObj getProperties() const override {
        mongo::extension::MongoExtensionStaticProperties properties;
        mongo::BSONObjBuilder builder;
        properties.setRequiresInputDocSource(false);
        properties.setPosition(mongo::extension::MongoExtensionPositionRequirementEnum::kFirst);
        properties.setHostType(mongo::extension::MongoExtensionHostTypeRequirementEnum::kAnyShard);
        properties.serialize(&builder);
        return builder.obj();
    }

    MongoExtensionFirstStageViewApplicationPolicy getFirstStageViewApplicationPolicy()
        const override {
        return MongoExtensionFirstStageViewApplicationPolicy::kDoNothing;
    }

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        return std::make_unique<ExtensionSearchMetaAstNode>(getName(), _arguments);
    }
};

DEFAULT_PARSE_NODE(ExtensionSearchMeta)

class SearchMetaParseNode : public SearchParseNodeBase {
public:
    SearchMetaParseNode() : SearchParseNodeBase("$searchMeta", {}) {}
    SearchMetaParseNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : SearchParseNodeBase(stageName, arguments) {}

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<SearchMetaParseNode>(getName(), _arguments);
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> expanded;
        expanded.reserve(1);
        expanded.emplace_back(new sdk::ExtensionAggStageAstNodeAdapter(
            std::make_unique<ExtensionSearchMetaAstNode>("$_extensionSearchMeta", _arguments)));
        return expanded;
    }
};

using SearchMetaStageDescriptor = sdk::TestStageDescriptor<"$searchMeta", SearchMetaParseNode>;
using ExtensionSearchMetaStageDescriptor =
    sdk::TestStageDescriptor<"$_extensionSearchMeta", ExtensionSearchMetaParseNode>;

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
