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

#include <string_view>

namespace sdk = mongo::extension::sdk;

namespace {
constexpr std::string_view kOptPrecondition = "optimization_precondition";
constexpr std::string_view kOptTransform = "optimization_transform";

void assertWithSpec(const mongo::BSONObj& obj) {
    auto assertionType = obj["assertionType"].valueStringDataSafe();
    if (assertionType != "uassert" && assertionType != "tassert") {
        // If we got here, just throw a generic C++ error.
        throw std::runtime_error("Generic error");
    }

    auto errmsg = obj["errmsg"].valueStringDataSafe();
    auto code = obj["code"].Number();

    // Check if it's a uassert.
    sdk_uassert(code, errmsg, assertionType != "uassert");

    // Check if it's a tassert.
    sdk_tassert(code, errmsg, assertionType != "tassert");
}
}  // namespace

DEFAULT_EXEC_STAGE(Assert);

class AssertLogicalStage : public sdk::TestLogicalStage<AssertExecStage> {
public:
    AssertLogicalStage(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::TestLogicalStage<AssertExecStage>(stageName, arguments) {}

    std::unique_ptr<sdk::LogicalAggStage> clone() const override {
        return std::make_unique<AssertLogicalStage>(_name, _arguments);
    }

    bool evaluatePipelineRewriteRulePrecondition(
        std::string_view ruleName,
        mongo::extension::ConstPipelineRewriteContextHandle) const override {
        auto phase = _arguments.getStringField("assertInPhase");
        if (ruleName == kOptPrecondition && phase == kOptPrecondition) {
            assertWithSpec(_arguments);
            return false;
        }
        if (ruleName == kOptTransform && phase == kOptTransform) {
            return true;
        }
        return false;
    }

    bool evaluatePipelineRewriteRuleTransform(
        std::string_view ruleName, mongo::extension::PipelineRewriteContextHandle) override {
        if (ruleName == kOptTransform &&
            _arguments.getStringField("assertInPhase") == kOptTransform) {
            assertWithSpec(_arguments);
        }
        return false;
    }
};

class AssertAstNode : public sdk::TestAstNode<AssertLogicalStage> {
public:
    AssertAstNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::TestAstNode<AssertLogicalStage>(stageName, arguments) {
        auto assertInPhase = _arguments.getStringField("assertInPhase");
        if (assertInPhase == "ast") {
            assertWithSpec(_arguments);
        }
    }

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        return std::make_unique<AssertAstNode>(getName(), _arguments);
    }
};

class AssertParseNode : public sdk::TestParseNode<AssertAstNode> {
public:
    AssertParseNode(std::string_view stageName, const mongo::BSONObj& stageBson)
        : sdk::TestParseNode<AssertAstNode>(stageName, stageBson),
          _stageBson(stageBson.getOwned()) {
        auto assertionType = _stageBson["assertionType"].valueStringDataSafe();
        if (assertionType == "uassert" || assertionType == "tassert") {
            auto errMsgElem = _stageBson["errmsg"];
            sdk_uassert(11569606,
                        "expected value for errmsg",
                        !errMsgElem.eoo() && errMsgElem.type() == mongo::BSONType::string);
            auto codeElem = _stageBson["code"];

            sdk_uassert(
                11569607, "expected value for code", !codeElem.eoo() && codeElem.isNumber());
        } else {
            sdk_uassert(11569608, "invalid value for assertionType", assertionType == "error");
        }

        // if we got here, assertInPhase must be empty, or ["parse", "ast",
        // "optimization_precondition", "optimization_transform"]
        auto assertInPhase = _stageBson.getStringField("assertInPhase");
        if (assertInPhase.empty() || assertInPhase == "parse") {
            assertWithSpec(_stageBson);
        }
        sdk_uassert(11569600,
                    "invalid value for assertInPhase",
                    assertInPhase == "ast" || assertInPhase == kOptPrecondition ||
                        assertInPhase == kOptTransform);
    }

    mongo::BSONObj getQueryShape(const sdk::QueryShapeOptsHandle&) const override {
        return _stageBson;
    }

    mongo::BSONObj toBsonForLog() const override {
        return _stageBson;
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<AssertParseNode>(getName(), _stageBson);
    }

protected:
    const mongo::BSONObj _stageBson;
};

/**
 * $assert is a no-op stage that asserts or errors in a specified lifecycle phase.
 *
 * Syntax:
 * {
 *  $assert: {
 *      assertionType: "uassert|tassert|error",
 *      errmsg: string,   // optional if assertionType is "error"
 *      code: number,     // optional if assertionType is "error"
 *      assertInPhase: "parse|ast|optimization_precondition|optimization_transform",
 *                        // optional; defaults to StageDescriptor::parse() when omitted
 *  }
 * }
 */
class AssertStageDescriptor : public sdk::TestStageDescriptor<"$assert", AssertParseNode> {
public:
    void validate(const mongo::BSONObj& arguments) const override {
        _assertGlobalObservabilityContextIsNull();
    }

private:
    /**
     * _assertGlobalObservabilityContextIsNull is a temporary test function that is used to confirm
     * that this a test extension that is loaded into the server should never see a valid
     * ObservabilityContext. Test extensions that are loaded into the server should be linked
     * statically, and should never call setGlobalObservabilityContext. As a result, the global
     * ObservabilityContext should always be null in an extension.
     */
    void _assertGlobalObservabilityContextIsNull() const {
        auto obsCtx = mongo::extension::getGlobalObservabilityContext();
        tassert(11569602,
                "Observability context was valid when it should not have been!",
                obsCtx == nullptr);
    }
};

class AssertExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<AssertStageDescriptor>(portal);
        _registerStageRules<AssertStageDescriptor>(
            portal,
            {
                {kOptPrecondition, mongo::extension::kReordering},
                {kOptTransform, mongo::extension::kReordering},
            });
    }
};
REGISTER_EXTENSION(AssertExtension)
DEFINE_GET_EXTENSION()
