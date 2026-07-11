// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/expression_function.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
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
}  // namespace
}  // namespace mongo
