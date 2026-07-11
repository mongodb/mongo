// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/pipeline/visitors/docs_needed_bounds.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <variant>

namespace mongo {
using namespace std::literals::string_view_literals;
TEST(DocsNeededBoundsTest, DocsNeededBoundsParsesCorrectly) {
    auto bsonElemFieldName = "docsNeeded"sv;

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
    auto bsonElemFieldName = "docsNeeded"sv;

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
