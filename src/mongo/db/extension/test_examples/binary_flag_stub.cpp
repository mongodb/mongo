// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

namespace sdk = mongo::extension::sdk;

/**
 * $testBinaryFlagStub is a no-op stage whose stage definition must be empty, i.e.
 * {$testBinaryFlagStub: {}}. Its entry in extension_stub_parsers.json is gated on the
 * binary-compatible featureFlagExtensionStubParsersBinary.
 */
using BinaryFlagStubStageDescriptor =
    sdk::TestStageDescriptor<"$testBinaryFlagStub",
                             sdk::shared_test_stages::TransformAggStageParseNode,
                             true /* ExpectEmptyStageDefinition */>;

DEFAULT_EXTENSION(BinaryFlagStub)
REGISTER_EXTENSION(BinaryFlagStubExtension)
DEFINE_GET_EXTENSION()
