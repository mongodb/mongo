// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

namespace sdk = mongo::extension::sdk;

struct ExtensionOptions {
    inline static bool optionA = false;
};

using OptionAStageDescriptor =
    sdk::TestStageDescriptor<"$optionA",
                             sdk::shared_test_stages::TransformAggStageParseNode,
                             true /* ExpectEmptyStageDefinition */>;
using OptionBStageDescriptor =
    sdk::TestStageDescriptor<"$optionB",
                             sdk::shared_test_stages::TransformAggStageParseNode,
                             true /* ExpectEmptyStageDefinition */>;

class MyExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        YAML::Node node = portal->getExtensionOptions();
        sdk_uassert(10999100, "Extension options must include 'optionA'", node["optionA"]);
        ExtensionOptions::optionA = node["optionA"].as<bool>();

        if (ExtensionOptions::optionA) {
            _registerStage<OptionAStageDescriptor>(portal);
        } else {
            _registerStage<OptionBStageDescriptor>(portal);
        }
    }
};

REGISTER_EXTENSION(MyExtension)
DEFINE_GET_EXTENSION()
