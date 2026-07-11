// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

namespace sdk = mongo::extension::sdk;

using FooStageDescriptor =
    sdk::TestStageDescriptor<"$foo", sdk::shared_test_stages::TransformAggStageParseNode>;
using BarStageDescriptor =
    sdk::TestStageDescriptor<"$bar", sdk::shared_test_stages::TransformAggStageParseNode>;

class MyExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<FooStageDescriptor>(portal);
        _registerStage<BarStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(MyExtension)
DEFINE_GET_EXTENSION()
