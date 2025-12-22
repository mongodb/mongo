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
#include "mongo/db/extension/public/extension_agg_stage_static_properties_gen.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/distributed_plan_logic.h"
#include "mongo/db/extension/sdk/dpl_array_container.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

namespace sdk = mongo::extension::sdk;

struct ToasterOptions {
    inline static double maxToasterHeat = 0;
    inline static bool allowBagels = false;
};

class ToastExecStage : public sdk::ExecAggStageSource {
public:
    ToastExecStage(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::ExecAggStageSource(stageName), _currentSlice(0) {
        _temp = arguments["temp"].Number();
        _numSlices = [&] {
            if (auto numSlices = arguments["numSlices"]) {
                return static_cast<int>(numSlices.Number());
            }
            return 1;
        }();
    }

    mongo::extension::ExtensionGetNextResult getNext(
        const mongo::extension::sdk::QueryExecutionContextHandle& execCtx,
        ::MongoExtensionExecAggStage* execStage) override {
        if (_currentSlice == _numSlices) {
            return mongo::extension::ExtensionGetNextResult::eof();
        }
        mongo::BSONObjBuilder builder;
        builder.append("slice", _currentSlice++);
        if (_temp < 300.0) {
            builder.append("notToasted", true);
        } else {
            builder.append("isBurnt", _temp > 400.0);
        }
        auto result = mongo::extension::ExtensionBSONObj::makeAsByteBuf(builder.obj());
        return mongo::extension::ExtensionGetNextResult::advanced(std::move(result));
    }

    void open() override {}
    void reopen() override {}
    void close() override {}
    mongo::BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return mongo::BSONObj();
    }

private:
    double _temp;
    int _numSlices;
    int _currentSlice;
};

class LoafExecStage : public sdk::TestExecStage {
public:
    LoafExecStage(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::TestExecStage(stageName, arguments),
          _numSlices([&] {
              if (auto numSlices = arguments["numSlices"]) {
                  return static_cast<int>(numSlices.Number());
              }
              return 1;
          }()),
          _currentSlice(0),
          _returnedLoaf(false) {}

    // Essentially functions like a $group stage (processes multiple input documents via
    // getNext() calls on the predecessor stage and outputs them in a single document).
    mongo::extension::ExtensionGetNextResult getNext(
        const mongo::extension::sdk::QueryExecutionContextHandle& execCtx,
        ::MongoExtensionExecAggStage* execStage) override {
        // Note that exec::agg::Stage::getNext() calls this getNext() method until it gets an eof.
        // So if we've already returned a batch of documents, _returnedLoaf should be true, and we
        // can return eof.
        if (_returnedLoaf) {
            return mongo::extension::ExtensionGetNextResult::eof();
        }
        mongo::BSONObjBuilder loafBuilder;
        while (_currentSlice < _numSlices) {
            auto input = _getSource()->getNext(execCtx.get());

            if (input.code == mongo::extension::GetNextCode::kPauseExecution) {
                return mongo::extension::ExtensionGetNextResult::pauseExecution();
            }

            if (input.code == mongo::extension::GetNextCode::kEOF) {
                // Return a partial loaf (this means the number of results returned by the
                // predecessor stage was less than the total number of slices (_numSlices) that
                // could have been processed).
                return _buildLoafResult(loafBuilder, "partialLoaf");
            }
            _appendSliceToLoaf(loafBuilder, input);
        }
        return _buildLoafResult(loafBuilder, "fullLoaf");
    }

private:
    int _numSlices;
    int _currentSlice;
    bool _returnedLoaf;

    void _appendSliceToLoaf(mongo::BSONObjBuilder& loafBuilder,
                            const mongo::extension::ExtensionGetNextResult& input) {
        // If we got here, we must have a document!
        sdk_tassert(10957500, "Failed to get an input document!", input.resultDocument.has_value());

        auto bsonObj = input.resultDocument->getUnownedBSONObj();
        mongo::BSONObjBuilder toastBuilder;
        toastBuilder.appendElements(bsonObj);
        // If the predecessor stage is $loaf, then we are dealing with directly nested loaves,
        // so use "loaf" instead of "slice". This is pretty meaningless, I just wanted to check that
        // the getName() logic works as expected.
        std::string keyPrefix = (_getSource()->getName() == getName()) ? "loaf" : "slice";
        loafBuilder.append(keyPrefix + std::to_string(_currentSlice++), toastBuilder.done());
    }

    mongo::extension::ExtensionGetNextResult _buildLoafResult(mongo::BSONObjBuilder& loafBuilder,
                                                              const std::string& loafType) {
        // Only return a loaf if at least one slice has been transformed.
        if (_currentSlice > 0 && !_returnedLoaf) {
            _returnedLoaf = true;
            auto returnedDoc = loafBuilder.done();
            return mongo::extension::ExtensionGetNextResult::advanced(
                mongo::extension::ExtensionBSONObj::makeAsByteBuf(BSON(loafType << returnedDoc)));
        }
        return mongo::extension::ExtensionGetNextResult::eof();
    }
};

DEFAULT_LOGICAL_STAGE(Toast);

class LoafLogicalStage : public sdk::TestLogicalStage<LoafExecStage> {
public:
    LoafLogicalStage(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::TestLogicalStage<LoafExecStage>(stageName, arguments) {}

    std::unique_ptr<sdk::LogicalAggStage> clone() const {
        return std::make_unique<LoafLogicalStage>(_name, _arguments);
    }

    boost::optional<sdk::DistributedPlanLogic> getDistributedPlanLogic() const override {
        sdk::DistributedPlanLogic dpl;
        // This stage must run on the merging node.
        {
            std::vector<mongo::extension::VariantDPLHandle> pipeline;
            pipeline.emplace_back(mongo::extension::LogicalAggStageHandle{
                new sdk::ExtensionLogicalAggStage(clone())});
            dpl.mergingPipeline = sdk::DPLArrayContainer(std::move(pipeline));
        }
        return dpl;
    }
};

class ToastAstNode : public sdk::TestAstNode<ToastLogicalStage> {
public:
    ToastAstNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::TestAstNode<ToastLogicalStage>(stageName, arguments) {}

    // TODO SERVER-114234 Set properties for this to be a collectionless stage.
    mongo::BSONObj getProperties() const override {
        mongo::extension::MongoExtensionStaticProperties properties;
        mongo::BSONObjBuilder builder;
        properties.setPosition(mongo::extension::MongoExtensionPositionRequirementEnum::kFirst);
        properties.setHostType(
            mongo::extension::MongoExtensionHostTypeRequirementEnum::kRunOnceAnyNode);
        properties.setRequiresInputDocSource(false);
        properties.serialize(&builder);
        return builder.obj();
    }
};

DEFAULT_AST_NODE(Loaf);

DEFAULT_PARSE_NODE(Toast);
DEFAULT_PARSE_NODE(Loaf);

/**
 * $loaf is a transform stage that requires a number of slices, like {$loaf: {numSlices: 5}}.
 * This stage processes N documents at a time and returns them where N <= numSlices.
 */
class LoafStageDescriptor : public sdk::TestStageDescriptor<"$loaf", LoafParseNode> {
public:
    void validate(const mongo::BSONObj& arguments) const override {
        if (auto numSlices = arguments["numSlices"]) {
            sdk_uassert(10957501,
                        "numSlices must be >= 0",
                        numSlices.isNumber() && numSlices.Number() >= 0);
        }
    }
};

/**
 * $toast is a source stage that requires a temperature and number of slices, like {$toast: {temp:
 * 3, numSlices: 5}}.
 */
class ToastStageDescriptor : public sdk::TestStageDescriptor<"$toast", ToastParseNode> {
public:
    void validate(const mongo::BSONObj& arguments) const override {
        sdk_uassert(11285301,
                    "expected temp input to " + kStageName,
                    arguments.hasField("temp") && arguments.getField("temp").isNumber());
        sdk_uassert(11285302,
                    "Failed to parse " + kStageName + ", provided temperature is higher than max " +
                        std::to_string(ToasterOptions::maxToasterHeat),
                    arguments["temp"].Number() <= ToasterOptions::maxToasterHeat);

        if (auto numSlices = arguments["numSlices"]) {
            sdk_uassert(10957601,
                        "numSlices must be >= 0",
                        numSlices.isNumber() && numSlices.Number() >= 0);
        }
    }
};

/**
 * $toastBagel is a no-op stage whose stage definition must be empty, like {$toastBagel: {}}.
 */
using ToastBagelStageDescriptor =
    sdk::TestStageDescriptor<"$toastBagel",
                             sdk::shared_test_stages::TransformAggStageParseNode,
                             true /* ExpectEmptyStageDefinition */>;

class ToasterExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        YAML::Node node = portal->getExtensionOptions();
        sdk_uassert(11285300,
                    "Extension options must include both 'maxToasterHeat' and 'allowBagels'",
                    node["maxToasterHeat"] && node["allowBagels"]);
        ToasterOptions::maxToasterHeat = node["maxToasterHeat"].as<double>();
        ToasterOptions::allowBagels = node["allowBagels"].as<bool>();

        // Always register $toast.
        _registerStage<ToastStageDescriptor>(portal);
        // Always register $loaf.
        _registerStage<LoafStageDescriptor>(portal);

        // Only register $toastBagel if allowBagels is true.
        if (ToasterOptions::allowBagels) {
            _registerStage<ToastBagelStageDescriptor>(portal);
        }
    }
};

REGISTER_EXTENSION(ToasterExtension)
DEFINE_GET_EXTENSION()
