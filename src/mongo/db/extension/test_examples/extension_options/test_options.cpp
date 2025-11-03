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

DEFAULT_LOGICAL_AST_PARSE(OptionA, "$optionA")
DEFAULT_LOGICAL_AST_PARSE(OptionB, "$optionB")
struct ExtensionOptions {
    inline static bool optionA = false;
};

/**
 * $optionA is a no-op stage.
 *
 * The stage definition must be empty, like {$optionA: {}}, or it will fail to parse.
 */
class OptionAStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(OptionAStageName);

    OptionAStageDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        sdk::validateStageDefinition(stageBson, kStageName, true /* checkEmpty */);

        return std::make_unique<OptionAParseNode>(stageBson);
    }
};

/**
 * $optionB is a no-op stage.
 *
 * The stage definition must be empty, like {$optionB: {}}, or it will fail to parse.
 */
class OptionBStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(OptionBStageName);

    OptionBStageDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        sdk::validateStageDefinition(stageBson, kStageName, true /* checkEmpty */);

        return std::make_unique<OptionBParseNode>(stageBson);
    }
};

class MyExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        YAML::Node node = portal.getExtensionOptions();
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
