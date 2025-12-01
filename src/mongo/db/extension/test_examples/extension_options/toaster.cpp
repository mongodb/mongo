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
#include "mongo/db/extension/public/extension_agg_stage_static_properties_gen.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"

namespace sdk = mongo::extension::sdk;

struct ToasterOptions {
    inline static double maxToasterHeat = 0;
    inline static bool allowBagels = false;
};

STAGE_NAME(Toast, "$toast");

class ToastExecStage : public sdk::ExecAggStageSource {
public:
    ToastExecStage(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::ExecAggStageSource(ToastStageName), _currentSlice(0) {
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

private:
    double _temp;
    int _numSlices;
    int _currentSlice;
};

DEFAULT_LOGICAL_STAGE(Toast);

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

DEFAULT_PARSE_NODE(Toast);

/**
 * $toast is a source stage that requires a temperature and number of slices, like {$toast: {temp:
 * 3, numSlices: 5}}.
 */
class ToastStageDescriptor : public sdk::TestStageDescriptor<ToastStageName, ToastParseNode> {
public:
    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        auto arguments = sdk::validateStageDefinition(stageBson, kStageName);

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

        return std::make_unique<ToastParseNode>(kStageName, std::move(arguments));
    }
};

DEFAULT_LOGICAL_AST_PARSE(ToastBagel, "$toastBagel")

/**
 * $toastBagel is a no-op stage whose stage definition must be empty, like {$toastBagel: {}}.
 */
using ToastBagelStageDescriptor = sdk::TestStageDescriptor<"$toastBagel", ToastBagelParseNode>;

class ToasterExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        YAML::Node node = portal.getExtensionOptions();
        sdk_uassert(11285300,
                    "Extension options must include both 'maxToasterHeat' and 'allowBagels'",
                    node["maxToasterHeat"] && node["allowBagels"]);
        ToasterOptions::maxToasterHeat = node["maxToasterHeat"].as<double>();
        ToasterOptions::allowBagels = node["allowBagels"].as<bool>();

        // Always register $toast.
        _registerStage<ToastStageDescriptor>(portal);

        // Only register $toastBagel if allowBagels is true.
        if (ToasterOptions::allowBagels) {
            _registerStage<ToastBagelStageDescriptor>(portal);
        }
    }
};

REGISTER_EXTENSION(ToasterExtension)
DEFINE_GET_EXTENSION()
