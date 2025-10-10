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

#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_util.h"

namespace sdk = mongo::extension::sdk;

DEFAULT_LOGICAL_AST_PARSE(Assert, "$assert")

/**
 * $assert is a no-op stage. It will assert or error at parse time based on the specified arguments.
 *
 * Syntax:
 * {
 *  $assert: {
 *      assertionType: "uassert|tassert|error",
 *      errmsg: string, // optional if assertionType is "error"
 *      code: number, // optional if assertionType is "error"
 *  }
 * }
 */
class AssertStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(AssertStageName);
    AssertStageDescriptor()
        : sdk::AggStageDescriptor(kStageName, MongoExtensionAggStageType::kNoOp) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        sdk::validateStageDefinition(stageBson, kStageName);

        const auto obj = stageBson.getField(kStageName).Obj();

        auto assertionType = obj["assertionType"].valueStringDataSafe();
        if (assertionType != "uassert" && assertionType != "tassert") {
            // If we got here, just throw a generic C++ error.
            throw std::runtime_error("Generic error");
            return nullptr;
        }

        auto errmsg = obj["errmsg"].valueStringDataSafe();
        auto code = obj["code"].Number();

        // Check if it's a uassert.
        userAssert(code, errmsg, assertionType != "uassert");

        // Check if it's a tassert.
        tripwireAssert(code, errmsg, assertionType != "tassert");

        return nullptr;
    }
};

class MyExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<AssertStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(MyExtension)
DEFINE_GET_EXTENSION()
