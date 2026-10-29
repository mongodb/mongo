// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

namespace sdk = mongo::extension::sdk;

/**
 * $testFoo is a no-op stage.
 *
 * This file is identical to foo.cpp except this stage does _not_ fail parsing if the
 * stage definition is empty. This is used for extension upgrade/downgrade testing.
 */
using FooStageDescriptor =
    sdk::TestStageDescriptor<"$testFoo",
                             sdk::shared_test_stages::TransformAggStageParseNode,
                             false /* ExpectEmptyStageDefinition */>;

DEFAULT_EXTENSION(Foo)
REGISTER_EXTENSION(FooExtension)
DEFINE_GET_EXTENSION()
