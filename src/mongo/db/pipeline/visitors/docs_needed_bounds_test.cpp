/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/db/pipeline/visitors/docs_needed_bounds.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <variant>

namespace mongo {
TEST(DocsNeededBoundsTest, DocsNeededBoundsParsesCorrectly) {
    auto bsonElemFieldName = "docsNeeded"_sd;

    auto bsonObj = BSON(bsonElemFieldName << "NeedAll");
    auto bounds = docs_needed_bounds::parseDocsNeededConstraintFromBSON(bsonObj[bsonElemFieldName]);
    ASSERT_TRUE(std::holds_alternative<docs_needed_bounds::NeedAll>(bounds));

    bsonObj = BSON(bsonElemFieldName << "Unknown");
    bounds = docs_needed_bounds::parseDocsNeededConstraintFromBSON(bsonObj[bsonElemFieldName]);
    ASSERT_TRUE(std::holds_alternative<docs_needed_bounds::Unknown>(bounds));

    bsonObj = BSON(bsonElemFieldName << 1);
    bounds = docs_needed_bounds::parseDocsNeededConstraintFromBSON(bsonObj[bsonElemFieldName]);
    ASSERT_TRUE(std::holds_alternative<long long>(bounds));
    ASSERT_EQ(std::get<long long>(bounds), 1);

    bsonObj = BSON(bsonElemFieldName << 9952);
    bounds = docs_needed_bounds::parseDocsNeededConstraintFromBSON(bsonObj[bsonElemFieldName]);
    ASSERT_TRUE(std::holds_alternative<long long>(bounds));
    ASSERT_EQ(std::get<long long>(bounds), 9952);
}

TEST(DocsNeededBoundsTest, DocsNeededBoundsParseErrors) {
    auto bsonElemFieldName = "docsNeeded"_sd;

    auto bsonObj = BSON(bsonElemFieldName << "Invalid");
    ASSERT_THROWS(docs_needed_bounds::parseDocsNeededConstraintFromBSON(bsonObj[bsonElemFieldName]),
                  ExceptionFor<ErrorCodes::BadValue>);

    bsonObj = BSON(bsonElemFieldName << 1.1);
    ASSERT_THROWS(docs_needed_bounds::parseDocsNeededConstraintFromBSON(bsonObj[bsonElemFieldName]),
                  ExceptionFor<ErrorCodes::BadValue>);

    bsonObj = BSON(bsonElemFieldName << Decimal128::kPositiveNaN);
    ASSERT_THROWS(docs_needed_bounds::parseDocsNeededConstraintFromBSON(bsonObj[bsonElemFieldName]),
                  ExceptionFor<ErrorCodes::BadValue>);

    bsonObj = BSON(bsonElemFieldName << -1);
    ASSERT_THROWS(docs_needed_bounds::parseDocsNeededConstraintFromBSON(bsonObj[bsonElemFieldName]),
                  ExceptionFor<ErrorCodes::BadValue>);
}

TEST(DocsNeededBoundsTest, DocsNeededBoundsSerializesCorrectly) {
    BSONObjBuilder objBuilder;

    docs_needed_bounds::serializeDocsNeededConstraint(
        docs_needed_bounds::Unknown(), "minBounds", &objBuilder);
    docs_needed_bounds::serializeDocsNeededConstraint(
        docs_needed_bounds::NeedAll(), "maxBounds", &objBuilder);
    docs_needed_bounds::serializeDocsNeededConstraint(101, "otherTestBounds", &objBuilder);

    ASSERT_BSONOBJ_EQ(objBuilder.done(),
                      BSON("minBounds" << "Unknown"
                                       << "maxBounds"
                                       << "NeedAll"
                                       << "otherTestBounds" << 101));
}
}  // namespace mongo
