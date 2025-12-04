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
