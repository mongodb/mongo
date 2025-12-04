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
        YAML::Node node = portal.getExtensionOptions();
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
