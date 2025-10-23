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
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_util.h"

namespace sdk = mongo::extension::sdk;

DEFAULT_LOGICAL_AST_PARSE(Toast, "$toast")
DEFAULT_LOGICAL_AST_PARSE(ToastBagel, "$toastBagel")

struct ToasterOptions {
    inline static double maxToasterHeat = 0;
    inline static bool allowBagels = false;
};

/**
 * $toast is a no-op stage that requires a temperature, like {$toast: {temp: 3}}.
 */
class ToastStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(ToastStageName);

    ToastStageDescriptor()
        : sdk::AggStageDescriptor(kStageName, MongoExtensionAggStageType::kNoOp) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        sdk::validateStageDefinition(stageBson, kStageName);

        const auto obj = stageBson.getField(kStageName).Obj();
        userAssert(11285301,
                   "Failed to parse " + kStageName + ", expected {" + kStageName +
                       ": {temp: <number>}}",
                   obj.hasField("temp") && obj.getField("temp").isNumber());

        userAssert(11285302,
                   "Failed to parse " + kStageName + ", provided temperature is higher than max " +
                       std::to_string(ToasterOptions::maxToasterHeat),
                   obj.getField("temp").numberDouble() <= ToasterOptions::maxToasterHeat);


        return std::make_unique<ToastParseNode>(stageBson);
    }
};

/**
 * $toastBagel is a no-op stage whose stage definition must be empty, like {$toastBagel: {}}.
 */
class ToastBagelStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(ToastBagelStageName);

    ToastBagelStageDescriptor()
        : sdk::AggStageDescriptor(kStageName, MongoExtensionAggStageType::kNoOp) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        sdk::validateStageDefinition(stageBson, kStageName, true /* checkEmpty */);

        return std::make_unique<ToastBagelParseNode>(stageBson);
    }
};

class ToasterExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        YAML::Node node = portal.getExtensionOptions();
        userAssert(11285300,
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
