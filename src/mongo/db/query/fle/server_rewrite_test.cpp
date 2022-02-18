/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <memory>

#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "server_rewrite.h"

namespace mongo {
namespace {

// Placeholder test for placeholder function to make sure compilation unit works.
// TODO SERVER-63294: replace this unit test with tests that check the actual behavior.
TEST(FLE2ServerRewrites, ServerRewriteIdentity) {
    auto me = std::make_unique<EqualityMatchExpression>("_id"_sd, Value(1));
    auto expected = EqualityMatchExpression("_id"_sd, Value(1));
    auto result = fle::rewriteMatchExpression(std::move(me));
    ASSERT_EQUALS(expected.path(), result->path());
}
}  // namespace
}  // namespace mongo
