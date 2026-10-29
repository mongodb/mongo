// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

    static const BSONObj kExpected = BSON(
        "$expr" << BSON(
            "$and" << BSON_ARRAY(BSON(
                "$eq" << BSON_ARRAY(
                    BSON("$getField" << BSON("input" << "$$ROOT"
                                                     << "field" << BSON("$literal" << "$golf")))
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
                     BSON("$eq" << BSON_ARRAY(
                              BSON("$getField"
                                   << BSON("input" << "$$ROOT"
                                                   << "field" << BSON("$literal" << "$golf")))
                              << BSON("$literal" << 7)))
                     << BSON("$eq" << BSON_ARRAY(
                                 BSON("$getField"
                                      << BSON("input" << "$$ROOT"
                                                      << "field" << BSON("$literal" << "$hotel")))
                                 << BSON("$literal" << BSON("$india" << BSON("$juliett" << 9)))))
                     << BSON("$eq" << BSON_ARRAY(
                                 BSON("$getField"
                                      << BSON("input" << "$$ROOT"
                                                      << "field" << BSON("$literal" << "$mike")))
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
                          BSON("$eq" << BSON_ARRAY(
                                   BSON("$getField"
                                        << BSON("input" << "$$ROOT"
                                                        << "field" << BSON("$literal" << "$golf")))
                                   << BSON("$literal" << 7)))
                          << BSON("$eq" << BSON_ARRAY(
                                      BSON("$getField" << BSON(
                                               "input" << "$$ROOT"
                                                       << "field" << BSON("$literal" << "$hotel")))
                                      << BSON("$literal"
                                              << BSON("$india" << BSON("$juliett" << 9)))))
                          << BSON("$eq" << BSON_ARRAY(
                                      BSON("$getField" << BSON(
                                               "input" << "$$ROOT"
                                                       << "field" << BSON("$literal" << "$mike")))
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
