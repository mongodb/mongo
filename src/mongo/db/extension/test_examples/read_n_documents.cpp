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
#include "mongo/db/extension/sdk/test_extension_factory.h"

namespace sdk = mongo::extension::sdk;
using namespace mongo;

class ProduceIdsExecStage : public sdk::ExecAggStageSource {
public:
    ProduceIdsExecStage(std::string_view stageName, const BSONObj& arguments)
        : sdk::ExecAggStageSource(stageName),
          _sortById(arguments["sortById"] && arguments["sortById"].booleanSafe()) {
        _numDocs = arguments["numDocs"] && arguments["numDocs"].isNumber()
            ? arguments["numDocs"].safeNumberInt()
            : 1;
    }

    extension::ExtensionGetNextResult getNext(const sdk::QueryExecutionContextHandle& execCtx,
                                              ::MongoExtensionExecAggStage* execStage) override {
        if (_currentDoc == _numDocs) {
            return extension::ExtensionGetNextResult::eof();
        }

        // Generate zero-indexed, ascending ids.
        auto currentId = _currentDoc++;
        auto document = extension::ExtensionBSONObj::makeAsByteBuf(BSON("_id" << currentId));

        if (!_sortById) {
            // We haven't been asked for sorted results, no need to generate sort key metadata.
            return extension::ExtensionGetNextResult::advanced(std::move(document));
        }

        // Generate sort key metadata so that the sharded sort by id can be applied correctly.
        auto metadata = extension::ExtensionBSONObj::makeAsByteBuf(
            BSON("$sortKey" << BSON("_id" << currentId)));
        return extension::ExtensionGetNextResult::advanced(std::move(document),
                                                           std::move(metadata));
    }

    void open() override {}
    void reopen() override {}
    void close() override {}
    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSONObj();
    }

private:
    const bool _sortById;
    int _numDocs;
    int _currentDoc = 0;
};

class ProduceIdsLogicalStage : public sdk::TestLogicalStage<ProduceIdsExecStage> {
public:
    ProduceIdsLogicalStage(std::string_view stageName, const BSONObj& arguments)
        : sdk::TestLogicalStage<ProduceIdsExecStage>(stageName, arguments) {}

    std::unique_ptr<sdk::LogicalAggStage> clone() const {
        return std::make_unique<ProduceIdsLogicalStage>(_name, _arguments);
    }

    boost::optional<extension::sdk::DistributedPlanLogic> getDistributedPlanLogic() const override {
        // We only need to provide distributed planning logic for correctness when we
        // need to sort by _id, but we specify it unconditionally to force more interesting
        // distributed planning.
        extension::sdk::DistributedPlanLogic dpl;

        {
            std::vector<extension::VariantDPLHandle> pipeline;
            pipeline.emplace_back(
                extension::LogicalAggStageHandle{new sdk::ExtensionLogicalAggStage(clone())});
            dpl.shardsPipeline = sdk::DPLArrayContainer(std::move(pipeline));
        }

        if (_arguments["sortById"] && _arguments["sortById"].booleanSafe()) {
            dpl.sortPattern = BSON("_id" << 1);
        }

        return dpl;
    }
};

class ProduceIdsAstNode : public sdk::TestAstNode<ProduceIdsLogicalStage> {
public:
    ProduceIdsAstNode(std::string_view stageName, const BSONObj& arguments)
        : sdk::TestAstNode<ProduceIdsLogicalStage>(stageName, arguments) {}

    BSONObj getProperties() const override {
        extension::MongoExtensionStaticProperties properties;
        properties.setPosition(extension::MongoExtensionPositionRequirementEnum::kFirst);
        properties.setRequiresInputDocSource(false);

        BSONObjBuilder builder;
        properties.serialize(&builder);
        return builder.obj();
    }
};

DEFAULT_PARSE_NODE(ProduceIds);

class ReadNDocumentsParseNode : public sdk::TestParseNode<ProduceIdsAstNode> {
public:
    ReadNDocumentsParseNode(std::string_view stageName, const BSONObj& arguments)
        : sdk::TestParseNode<ProduceIdsAstNode>(stageName, arguments) {}

    size_t getExpandedSize() const override {
        return 2;
    }

    std::vector<extension::VariantNodeHandle> expand() const override {
        std::vector<extension::VariantNodeHandle> expanded;
        expanded.reserve(getExpandedSize());
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(
            std::make_unique<ProduceIdsAstNode>("$produceIds", _arguments)));
        expanded.emplace_back(
            extension::sdk::HostServicesAPI::getInstance()->createIdLookup(kIdLookupSpec));
        return expanded;
    }

private:
    static const BSONObj kIdLookupSpec;
};

const BSONObj ReadNDocumentsParseNode::kIdLookupSpec =
    BSON("$_internalSearchIdLookup" << BSONObj());


/**
 * ReadNDocuments stage that will read documents with integer ids from a collection and will
 * optionally return them sorted by _id value (ascending). Syntax:
 *
 * {$readNDocuments: {numDocs: <int>, sortById: <optional bool>}}
 *
 * $readNDocuments will desugar to two stages: $produceIds, which will produce integer ids up to
 * 'numDocs', followed by $_internalSearchIdLookup, which will attempt to read documents with the
 * given ids from the collection.
 */
using ReadNDocumentsStageDescriptor =
    sdk::TestStageDescriptor<"$readNDocuments", ReadNDocumentsParseNode>;
// $produceIds needs to be parseable to be used in sharded execution.
using ProduceIdsStageDescriptor = sdk::TestStageDescriptor<"$produceIds", ProduceIdsParseNode>;

class ReadNDocumentsExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<ReadNDocumentsStageDescriptor>(portal);
        _registerStage<ProduceIdsStageDescriptor>(portal);
    }
};
REGISTER_EXTENSION(ReadNDocumentsExtension)
DEFINE_GET_EXTENSION()
