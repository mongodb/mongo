/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

// TODO SERVER-115137: Remove this test extension.

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

namespace sdk = mongo::extension::sdk;

using MongotHostVerifiedStageDescriptor =
    sdk::TestStageDescriptor<"$mongotHostVerified",
                             sdk::shared_test_stages::TransformAggStageParseNode,
                             true /* ExpectEmptyStageDefinition */>;

/**
 * Test extension that verifies mongotHost injection.
 * Registers $mongotHostVerified stage only if the injected mongotHost matches expectedMongotHost.
 */
class MongotHostTestExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        auto node = portal->getExtensionOptions();

        sdk_uassert(11513600,
                    "Extension options must include 'expectedMongotHost'",
                    node["expectedMongotHost"]);

        std::string expected = node["expectedMongotHost"].as<std::string>();
        sdk_uassert(11513601, "'expectedMongotHost' must not be empty", !expected.empty());

        std::string actual = node["mongotHost"] ? node["mongotHost"].as<std::string>() : "";

        // Register $mongotHostVerified only if mongotHost matches the expected value.
        if (actual == expected) {
            _registerStage<MongotHostVerifiedStageDescriptor>(portal);
        }
    }
};

REGISTER_EXTENSION(MongotHostTestExtension)
DEFINE_GET_EXTENSION()
