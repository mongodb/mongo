// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

namespace sdk = mongo::extension::sdk;

struct ExtensionOptions {
    inline static bool checkMax = false;
    inline static double max = -1;
};


/**
 * $checkNum is a no-op stage.
 *
 * The stage definition must include a "num" field, like {$checkNum: {num: <double>}}, or it will
 * fail to parse. If 'checkMax' is true and the supplied num is greater than 'max', it will fail to
 * parse.
 */
class CheckNumStageDescriptor
    : public sdk::TestStageDescriptor<"$checkNum",
                                      sdk::shared_test_stages::TransformAggStageParseNode> {
public:
    void validate(const mongo::BSONObj& arguments) const override {
        sdk_uassert(10999105,
                    "Failed to parse " + kStageName + ", expected {" + kStageName +
                        ": {num: <double>}}",
                    arguments.hasField("num") && arguments.getField("num").isNumber());

        if (ExtensionOptions::checkMax) {
            sdk_uassert(10999106,
                        "Failed to parse " + kStageName + ", provided num is higher than max " +
                            std::to_string(ExtensionOptions::max),
                        arguments.getField("num").numberDouble() <= ExtensionOptions::max);
        }
    }
};

class MyExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        YAML::Node node = portal->getExtensionOptions();
        sdk_uassert(10999107, "Extension options must include 'checkMax'", node["checkMax"]);
        ExtensionOptions::checkMax = node["checkMax"].as<bool>();
        if (ExtensionOptions::checkMax) {
            sdk_uassert(10999103, "Extension options must include 'max'", node["max"]);
            ExtensionOptions::max = node["max"].as<double>();
        }
        _registerStage<CheckNumStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(MyExtension)
DEFINE_GET_EXTENSION()
