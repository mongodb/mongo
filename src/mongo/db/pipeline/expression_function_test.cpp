// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/expression_function.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

#include <functional>


namespace mongo {

namespace {

TEST(ExpressionFunction, SerializeAndRedactArgs) {
    query_shape::SerializationOptions options =
        query_shape::SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;

    auto expCtx = ExpressionContextForTest();
    auto expr = BSON("$function" << BSON("body" << "function(age) {return age >= 21;}"
                                                << "args" << BSON_ARRAY("$age") << "lang"
                                                << "js"));
    VariablesParseState vps = expCtx.variablesParseState;
    auto exprFunc = ExpressionFunction::parse(&expCtx, expr.firstElement(), vps);
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"$function":{"body":"?string","args":["$HASH<age>"],"lang":"js"}})",
        exprFunc->serialize(options).getDocument());
}

class ExpressionFunctionInternalClientTest : public ServiceContextTest {
protected:
    struct ClientAndOpCtx {
        ServiceContext::UniqueClient client;
        ServiceContext::UniqueOperationContext opCtx;
    };

    ClientAndOpCtx makeClientAndOpCtx(bool isInternalClient) {
        auto client = getService()->makeClient("ExpressionFunctionInternalClientTest",
                                               _transportLayer.createSession());
        client->setIsInternalClient(isInternalClient);
        auto opCtx = getServiceContext()->makeOperationContext(client.get());
        return {std::move(client), std::move(opCtx)};
    }

    static BSONObj functionWithInternalFlag() {
        return BSON("$function" << BSON("body" << "function(a) { return true; }" << "args"
                                               << BSON_ARRAY("$$CURRENT") << "lang" << "js"
                                               << "_internalSetObjToThis" << true));
    }

private:
    transport::TransportLayerMock _transportLayer;
};

TEST_F(ExpressionFunctionInternalClientTest, ExternalClientCannotSetInternalSetObjToThis) {
    auto clientAndOpCtx = makeClientAndOpCtx(/*isInternalClient*/ false);
    ExpressionContextForTest expCtx(clientAndOpCtx.opCtx.get());
    auto expr = functionWithInternalFlag();
    VariablesParseState vps = expCtx.variablesParseState;
    ASSERT_THROWS_CODE(
        ExpressionFunction::parse(&expCtx, expr.firstElement(), vps), AssertionException, 13011000);
}

TEST_F(ExpressionFunctionInternalClientTest, InternalClientCanSetInternalSetObjToThis) {
    auto clientAndOpCtx = makeClientAndOpCtx(/*isInternalClient*/ true);
    ExpressionContextForTest expCtx(clientAndOpCtx.opCtx.get());
    auto expr = functionWithInternalFlag();
    VariablesParseState vps = expCtx.variablesParseState;
    auto exprFunc = ExpressionFunction::parse(&expCtx, expr.firstElement(), vps);
    ASSERT(exprFunc);
}

TEST_F(ExpressionFunctionInternalClientTest, SessionlessClientCanSetInternalSetObjToThis) {
    auto client = getService()->makeClient("ExpressionFunctionInternalClientTest");
    ASSERT_FALSE(client->session());
    auto opCtx = getServiceContext()->makeOperationContext(client.get());
    ExpressionContextForTest expCtx(opCtx.get());
    auto expr = functionWithInternalFlag();
    VariablesParseState vps = expCtx.variablesParseState;
    auto exprFunc = ExpressionFunction::parse(&expCtx, expr.firstElement(), vps);
    ASSERT(exprFunc);
}

TEST_F(ExpressionFunctionInternalClientTest,
       ExternalClientCanUseFunctionWithoutInternalSetObjToThis) {
    auto clientAndOpCtx = makeClientAndOpCtx(/*isInternalClient*/ false);
    ExpressionContextForTest expCtx(clientAndOpCtx.opCtx.get());
    auto expr = BSON("$function" << BSON("body" << "function(a) { return true; }" << "args"
                                                << BSON_ARRAY("$$CURRENT") << "lang" << "js"));
    VariablesParseState vps = expCtx.variablesParseState;
    auto exprFunc = ExpressionFunction::parse(&expCtx, expr.firstElement(), vps);
    ASSERT(exprFunc);
}
}  // namespace
}  // namespace mongo
