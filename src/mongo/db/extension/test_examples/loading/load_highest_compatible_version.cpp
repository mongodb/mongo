// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

namespace sdk = mongo::extension::sdk;

// Defines a complete extension version (Extension, StageDescriptor, ParseNode, and LogicalStage).
#define DEFINE_EXTENSION_VERSION(VERSION_NUM)                                          \
    using ExtensionV##VERSION_NUM##StageDescriptor =                                   \
        sdk::TestStageDescriptor<"$extensionV" #VERSION_NUM,                           \
                                 sdk::shared_test_stages::TransformAggStageParseNode>; \
    DEFAULT_EXTENSION(ExtensionV##VERSION_NUM);

// Generate code for 3 extension versions, all with unique stage names.
DEFINE_EXTENSION_VERSION(1)
DEFINE_EXTENSION_VERSION(2)
DEFINE_EXTENSION_VERSION(3)

// v1 is the base version, v2 increments the minor version, and v3 increments
// the major version (not compatible). We register the extensions in this odd order to make sure the
// set is sorting and not just getting lucky with placement.
REGISTER_EXTENSION_WITH_VERSION(ExtensionV2Extension,
                                (::MongoExtensionAPIVersion{MONGODB_EXTENSION_API_MAJOR_VERSION,
                                                            MONGODB_EXTENSION_API_MINOR_VERSION +
                                                                1}))
REGISTER_EXTENSION_WITH_VERSION(ExtensionV3Extension,
                                (::MongoExtensionAPIVersion{MONGODB_EXTENSION_API_MAJOR_VERSION + 1,
                                                            MONGODB_EXTENSION_API_MINOR_VERSION}))
REGISTER_EXTENSION_WITH_VERSION(ExtensionV1Extension, (MONGODB_EXTENSION_API_VERSION))
DEFINE_GET_EXTENSION()
