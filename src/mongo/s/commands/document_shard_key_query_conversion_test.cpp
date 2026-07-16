/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/s/commands/document_shard_key_query_conversion.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ConvertDocumentIntoQueryTest, DocumentWithoutDollarFieldsIsReturnedAsIs) {
    static const BSONObj kDocument =
        BSON("x" << 4 << "y" << 3 << "z" << BSON("a" << 2 << "b" << 1) << "_id" << 20);

    ASSERT_BSONOBJ_EQ(kDocument, convertDocumentIntoQuery(kDocument));
}

TEST(ConvertDocumentIntoQueryTest, EmptyDocumentProducesEmptyQuery) {
    ASSERT_BSONOBJ_EQ(BSONObj(), convertDocumentIntoQuery(BSONObj()));
}

TEST(ConvertDocumentIntoQueryTest, DollarPrefixedObjectValueIsWrappedInEq) {
    static const BSONObj kDocument =
        BSON("obj" << BSON("$beta" << 4) << "obj2"
                   << BSON("$charlie" << 5 << "$delta" << BSON("$foxtrot" << 6)));

    static const BSONObj kExpected =
        BSON("obj" << BSON("$eq" << BSON("$beta" << 4)) << "obj2"
                   << BSON("$eq" << BSON("$charlie" << 5 << "$delta" << BSON("$foxtrot" << 6))));

    ASSERT_BSONOBJ_EQ(kExpected, convertDocumentIntoQuery(kDocument));
}

TEST(ConvertDocumentIntoQueryTest, NonObjectValueIsNotWrappedInEqEvenIfFirstCharIsDollar) {
    static const BSONObj kDocument = BSON("arr" << BSON_ARRAY(2 << BSON("$alpha" << 3)) << "obj3"
                                                << BSON("subobj" << BSON("$kilo" << 10)));

    ASSERT_BSONOBJ_EQ(kDocument, convertDocumentIntoQuery(kDocument));
}

TEST(ConvertDocumentIntoQueryTest, DollarPrefixedFieldNameIsRewrittenIntoExprGetField) {
    static const BSONObj kDocument = BSON("$golf" << 7);

    static const BSONObj kExpected =
        BSON("$expr" << BSON("$and" << BSON_ARRAY(BSON(
                                 "$eq" << BSON_ARRAY(BSON("$getField" << BSON("input"
                                                                              << "$$ROOT"
                                                                              << "field"
                                                                              << BSON("$literal"
                                                                                      << "$golf")))
                                                     << BSON("$literal" << 7))))));

    ASSERT_BSONOBJ_EQ(kExpected, convertDocumentIntoQuery(kDocument));
}

TEST(ConvertDocumentIntoQueryTest, MultipleDollarPrefixedFieldNamesAreAndedTogether) {
    static const BSONObj kDocument =
        BSON("$golf" << 7 << "$hotel" << BSON("$india" << BSON("$juliett" << 9)) << "$mike"
                     << BSON_ARRAY(11 << 12));

    static const BSONObj kExpected =
        BSON("$expr" << BSON(
                 "$and" << BSON_ARRAY(
                     BSON("$eq" << BSON_ARRAY(BSON("$getField" << BSON("input"
                                                                       << "$$ROOT"
                                                                       << "field"
                                                                       << BSON("$literal"
                                                                               << "$golf")))
                                              << BSON("$literal" << 7)))
                     << BSON("$eq" << BSON_ARRAY(
                                 BSON("$getField" << BSON("input"
                                                          << "$$ROOT"
                                                          << "field"
                                                          << BSON("$literal"
                                                                  << "$hotel")))
                                 << BSON("$literal" << BSON("$india" << BSON("$juliett" << 9)))))
                     << BSON("$eq" << BSON_ARRAY(BSON("$getField" << BSON("input"
                                                                          << "$$ROOT"
                                                                          << "field"
                                                                          << BSON("$literal"
                                                                                  << "$mike")))
                                                 << BSON("$literal" << BSON_ARRAY(11 << 12)))))));

    ASSERT_BSONOBJ_EQ(kExpected, convertDocumentIntoQuery(kDocument));
}

TEST(ConvertDocumentIntoQueryTest, MixOfPlainDollarObjectAndDollarFieldNames) {
    static const BSONObj kDocument = BSON(
        "_id" << 1 << "array" << BSON_ARRAY(2 << BSON("$alpha" << 3)) << "obj" << BSON("$beta" << 4)
              << "obj2" << BSON("$charlie" << 5 << "$delta" << BSON("$foxtrot" << 6)) << "$golf"
              << 7 << "$hotel" << BSON("$india" << BSON("$juliett" << 9)) << "obj3"
              << BSON("subobj" << BSON("$kilo" << 10)) << "$mike" << BSON_ARRAY(11 << 12));

    static const BSONObj kExpected = BSON(
        "_id" << 1 << "array" << BSON_ARRAY(2 << BSON("$alpha" << 3)) << "obj"
              << BSON("$eq" << BSON("$beta" << 4)) << "obj2"
              << BSON("$eq" << BSON("$charlie" << 5 << "$delta" << BSON("$foxtrot" << 6))) << "obj3"
              << BSON("subobj" << BSON("$kilo" << 10)) << "$expr"
              << BSON("$and" << BSON_ARRAY(
                          BSON("$eq" << BSON_ARRAY(BSON("$getField" << BSON("input"
                                                                            << "$$ROOT"
                                                                            << "field"
                                                                            << BSON("$literal"
                                                                                    << "$golf")))
                                                   << BSON("$literal" << 7)))
                          << BSON("$eq"
                                  << BSON_ARRAY(BSON("$getField" << BSON("input"
                                                                         << "$$ROOT"
                                                                         << "field"
                                                                         << BSON("$literal"
                                                                                 << "$hotel")))
                                                << BSON("$literal" << BSON(
                                                            "$india" << BSON("$juliett" << 9)))))
                          << BSON("$eq"
                                  << BSON_ARRAY(BSON("$getField" << BSON("input"
                                                                         << "$$ROOT"
                                                                         << "field"
                                                                         << BSON("$literal"
                                                                                 << "$mike")))
                                                << BSON("$literal" << BSON_ARRAY(11 << 12)))))));

    ASSERT_BSONOBJ_EQ(kExpected, convertDocumentIntoQuery(kDocument));
}

TEST(ContainsDollarPrefixedFieldNamesOnTopLevelTest, EmptyDocumentReturnsFalse) {
    ASSERT_FALSE(containsDollarPrefixedFieldNamesOnTopLevel(Document()));
}

TEST(ContainsDollarPrefixedFieldNamesOnTopLevelTest, DocumentWithoutDollarFieldsReturnsFalse) {
    Document doc =
        Document{BSON("x" << 4 << "y" << 3 << "z" << BSON("a" << 2 << "b" << 1) << "_id" << 20)};

    ASSERT_FALSE(containsDollarPrefixedFieldNamesOnTopLevel(doc));
}

TEST(ContainsDollarPrefixedFieldNamesOnTopLevelTest,
     NonObjectValueStartingWithDollarDoesNotCountAsDollarPrefixedObjectValue) {
    Document doc = Document{BSON("arr" << BSON_ARRAY(2 << BSON("$alpha" << 3)) << "obj3"
                                       << BSON("subobj" << BSON("$kilo" << 10)))};

    ASSERT_FALSE(containsDollarPrefixedFieldNamesOnTopLevel(doc));
}

TEST(ContainsDollarPrefixedFieldNamesOnTopLevelTest, DollarPrefixedObjectValueReturnsTrue) {
    Document doc = Document{BSON("obj" << BSON("$beta" << 4))};

    ASSERT_TRUE(containsDollarPrefixedFieldNamesOnTopLevel(doc));
}

TEST(ContainsDollarPrefixedFieldNamesOnTopLevelTest, DollarPrefixedFieldNameReturnsTrue) {
    Document doc = Document{BSON("$golf" << 7)};

    ASSERT_TRUE(containsDollarPrefixedFieldNamesOnTopLevel(doc));
}

TEST(ContainsDollarPrefixedFieldNamesOnTopLevelTest, MixOfPlainAndDollarPrefixedFieldsReturnsTrue) {
    Document doc = Document{BSON("_id" << 1 << "array" << BSON_ARRAY(2 << BSON("$alpha" << 3))
                                       << "obj3" << BSON("subobj" << BSON("$kilo" << 10)) << "$mike"
                                       << BSON_ARRAY(11 << 12))};

    ASSERT_TRUE(containsDollarPrefixedFieldNamesOnTopLevel(doc));
}

}  // namespace
}  // namespace mongo
