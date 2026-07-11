// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

TEST(InternalSchemaXorOp, Equivalent) {
    BSONObj baseOperand1 = BSON("a" << 1);
    BSONObj baseOperand2 = BSON("b" << 2);
    EqualityMatchExpression sub1("a"sv, baseOperand1["a"]);
    EqualityMatchExpression sub2("b"sv, baseOperand2["b"]);

    InternalSchemaXorMatchExpression e1;
    e1.add(sub1.clone());
    e1.add(sub2.clone());

    InternalSchemaXorMatchExpression e2;
    e2.add(sub1.clone());

    ASSERT(e1.equivalent(&e1));
    ASSERT_FALSE(e1.equivalent(&e2));
}
}  // namespace
}  // namespace mongo
