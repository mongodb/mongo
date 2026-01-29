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
#include "mongo/util/testing_proctor.h"

namespace sdk = mongo::extension::sdk;

namespace {
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
DEFAULT_LOGICAL_STAGE(Assert);

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

        // if we got here, assertInPhase must be empty, or ["parse", "ast"]
        auto assertInPhase = _stageBson.getStringField("assertInPhase");
        if (assertInPhase.empty() || assertInPhase == "parse") {
            assertWithSpec(_stageBson);
        }
        sdk_uassert(11569600, "invalid value for assertInPhase", assertInPhase == "ast");
    }

    mongo::BSONObj getQueryShape(const sdk::QueryShapeOptsHandle&) const override {
        return _stageBson;
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<AssertParseNode>(getName(), _stageBson);
    }

protected:
    const mongo::BSONObj _stageBson;
};

/**
 * $assert is a no-op stage. It will assert or error at parse time based on the specified arguments.
 *
 * Syntax:
 * {
 *  $assert: {
 *      assertionType: "uassert|tassert|error",
 *      errmsg: string, // optional if assertionType is "error"
 *      code: number, // optional if assertionType is "error"
 *      assertInPhase: string, // optional, specifies what phase of processing to assert. If not
 *                              specified, asserts in StageDescriptor::parse().
 *  }
 * }
 */
class AssertStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string("$assert");

    AssertStageDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        _assertGlobalObservabilityContextIsNull();
        auto obj = sdk::validateStageDefinition(stageBson, kStageName);
        return std::make_unique<AssertParseNode>(kStageName, obj);
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
        /**
         * TODO SERVER-115700: Unfortunately, when extensions are built as part of a unit test,
         * they are not yet statically linked. This means that in unit tests, it's possible that
         * we may have an observability context set due to the host and extension code
         * dynamically linking against the same sdk. For now, disable this check when running
         * from a unit test.
         *
         * Note: when an extension is statically linked, the TestingProctor is not initialized,
         * because mongo::GlobalInitializers are never run. Therefore, in this case, it follows that
         * the extension is likely being loaded into the server, so we can safely assert that we
         * have a null ObservabilityContext.
         */
        if (mongo::TestingProctor::instance().isInitialized() &&
            mongo::TestingProctor::instance().isEnabled()) {
            return;
        }
        auto obsCtx = mongo::extension::getGlobalObservabilityContext();
        tassert(11569602,
                "Observability context was valid when it should not have been!",
                obsCtx == nullptr);
    }
};

DEFAULT_EXTENSION(Assert)
REGISTER_EXTENSION(AssertExtension)
DEFINE_GET_EXTENSION()
