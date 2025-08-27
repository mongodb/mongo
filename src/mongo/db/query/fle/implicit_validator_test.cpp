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

#include "mongo/db/query/fle/implicit_validator.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/json.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {

FleBlobHeader makeFleHeader(const EncryptedField& field, EncryptedBinDataType subtype) {
    FleBlobHeader blob;
    blob.fleBlobSubtype = static_cast<uint8_t>(subtype);
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    ASSERT(field.getBsonType().has_value());
    blob.originalBsonType = stdx::to_underlying(typeFromName(field.getBsonType().value()));
    return blob;
}

BSONBinData makeFleBinData(const FleBlobHeader& blob) {
    return BSONBinData(
        reinterpret_cast<const void*>(&blob), sizeof(FleBlobHeader), BinDataType::Encrypt);
}

const UUID kTestKeyId = UUID::parse("deadbeef-0000-0000-0000-0000deadbeef").getValue();


void replace_str(std::string& str, StringData search, StringData replace) {
    auto pos = str.find(search.data(), 0, search.size());
    if (pos == std::string::npos)
        return;
    str.replace(pos, search.size(), replace.data(), replace.size());
}

void replace_all(std::string& str, StringData search, StringData replace) {
    auto pos = str.find(search.data(), 0, search.size());
    while (pos != std::string::npos) {
        str.replace(pos, search.size(), replace.data(), replace.size());
        pos += replace.size();
        pos = str.find(search.data(), pos, search.size());
    }
}

std::string expectedLeafExpr(const EncryptedField& field) {
    std::string tmpl = R"(
            {"$or":[
                {"<NAME>":{"$not":{"$exists":true}}},
                {"$and":[
                    {"<NAME>":{"$_internalSchemaBinDataFLE2EncryptedType":[<TYPE>]}}
                ]}
            ]})";
    FieldRef ref(field.getPath());
    replace_all(tmpl, "<NAME>"_sd, ref.getPart(ref.numParts() - 1));
    if (field.getBsonType().has_value()) {
        // {"$numberInt":"<TYPE>"}
        replace_all(tmpl,
                    "<TYPE>"_sd,
                    str::stream() << "{\"$numberInt\":\""
                                  << static_cast<int>(typeFromName(field.getBsonType().value()))
                                  << "\"}");
    } else {
        replace_all(tmpl, "<TYPE>"_sd, "");
    }
    return tmpl;
}

std::string expectedNonLeafExpr(StringData fieldName, StringData subschema) {
    std::string tmpl = R"(
            {"$or":[
                {"<NAME>":{"$not":{"$exists":true}}},
                {"$and":[
                    {"$or":[
                        {"<NAME>":{"$not":{"$_internalSchemaType":[{"$numberInt":"3"}]}}},
                        {"<NAME>":{"$_internalSchemaObjectMatch":<SUBSCHEMA>}}
                    ]},
                    {"<NAME>":{"$not":{"$_internalSchemaType":[{"$numberInt":"4"}]}}}
                ]}
            ]})";
    replace_all(tmpl, "<NAME>"_sd, fieldName);
    replace_all(tmpl, "<SUBSCHEMA>"_sd, subschema);
    return tmpl;
}

class GenerateFLE2MatchExpression : public unittest::Test {
public:
    GenerateFLE2MatchExpression() {
        kFieldAbc.setBsonType("string"_sd);
        kFieldAbd.setBsonType("int"_sd);
        kFieldC.setBsonType("array"_sd);
        kFieldAxy.setBsonType("bool"_sd);

        kValueAbc = makeFleHeader(kFieldAbc, EncryptedBinDataType::kFLE2EqualityIndexedValue);
        kValueAbd = makeFleHeader(kFieldAbd, EncryptedBinDataType::kFLE2EqualityIndexedValue);
        kValueC = makeFleHeader(kFieldC, EncryptedBinDataType::kFLE2EqualityIndexedValue);
        kValueAxy = makeFleHeader(kFieldAxy, EncryptedBinDataType::kFLE2EqualityIndexedValue);
        // Use the type information from kFieldAbc
        kValueAxt = makeFleHeader(kFieldAbc, EncryptedBinDataType::kFLE2UnindexedEncryptedValue);
        kValueFLE1 = makeFleHeader(kFieldAbc, EncryptedBinDataType::kDeterministic);

        kEncryptedFields =
            std::vector<EncryptedField>{kFieldAbc, kFieldAbd, kFieldC, kFieldAxy, kFieldAxt};
    }

    EncryptedField kFieldAbc{kTestKeyId, "a.b.c"};
    EncryptedField kFieldAbd{kTestKeyId, "a.b.d"};
    EncryptedField kFieldC{kTestKeyId, "c"};
    EncryptedField kFieldAxy{kTestKeyId, "a.x.y"};
    EncryptedField kFieldAxt{kTestKeyId, "a.x.t"};  // Untyped

    FleBlobHeader kValueAbc;
    FleBlobHeader kValueAbd;
    FleBlobHeader kValueC;
    FleBlobHeader kValueAxy;
    FleBlobHeader kValueAxt;
    FleBlobHeader kValueFLE1;

    std::vector<EncryptedField> kEncryptedFields;
};


TEST_F(GenerateFLE2MatchExpression, EmptyInput) {
    auto swExpr = generateMatchExpressionFromEncryptedFields(new ExpressionContextForTest(), {});
    ASSERT(swExpr.isOK());
    ASSERT_BSONOBJ_EQ(mongo::fromjson("{$alwaysTrue: 1}"), swExpr.getValue()->serialize());
}

TEST_F(GenerateFLE2MatchExpression, SimpleInput) {
    EncryptedField foo(UUID::gen(), "foo");
    foo.setBsonType("string"_sd);
    EncryptedField bar(UUID::gen(), "bar");
    bar.setBsonType("string"_sd);

    std::string expectedJSON = R"({"$and":[
        {"$and":[
            <fooExpr>,
            <barExpr>
        ]}
    ]})";

    auto swExpr =
        generateMatchExpressionFromEncryptedFields(new ExpressionContextForTest(), {foo, bar});
    ASSERT(swExpr.isOK());
    auto outputBSON = swExpr.getValue()->serialize();

    replace_str(expectedJSON, "<fooExpr>", expectedLeafExpr(foo));
    replace_str(expectedJSON, "<barExpr>", expectedLeafExpr(bar));

    auto expectedBSON = mongo::fromjson(expectedJSON);
    ASSERT_BSONOBJ_EQ(expectedBSON, outputBSON);
}

TEST_F(GenerateFLE2MatchExpression, NormalInputWithNestedFields) {
    auto swExpr = generateMatchExpressionFromEncryptedFields(new ExpressionContextForTest(),
                                                             kEncryptedFields);
    ASSERT(swExpr.isOK());

    auto outputBSON = swExpr.getValue()->serialize();

    std::string rootSchema = R"({"$and":[{"$and":[<aNonLeafExpr>, <cLeafExpr>]}]})";
    std::string aSubschema = R"({"$and":[<abNonLeafExpr>, <axNonLeafExpr>]})";
    std::string abSubschema = R"({"$and":[<abcLeafExpr>, <abdLeafExpr>]})";
    std::string axSubschema = R"({"$and":[<axyLeafExpr>, <axtLeafExpr>]})";

    replace_str(rootSchema, "<cLeafExpr>", expectedLeafExpr(kFieldC));
    replace_str(rootSchema, "<aNonLeafExpr>", expectedNonLeafExpr("a", aSubschema));
    replace_all(rootSchema, "<abNonLeafExpr>", expectedNonLeafExpr("b", abSubschema));
    replace_all(rootSchema, "<axNonLeafExpr>", expectedNonLeafExpr("x", axSubschema));
    replace_all(rootSchema, "<abcLeafExpr>", expectedLeafExpr(kFieldAbc));
    replace_all(rootSchema, "<abdLeafExpr>", expectedLeafExpr(kFieldAbd));
    replace_all(rootSchema, "<axyLeafExpr>", expectedLeafExpr(kFieldAxy));
    replace_all(rootSchema, "<axtLeafExpr>", expectedLeafExpr(kFieldAxt));

    auto expectedBSON = mongo::fromjson(rootSchema);
    ASSERT_BSONOBJ_EQ(expectedBSON, outputBSON);
}

DEATH_TEST(GenerateFLE2MatchExpressionDeathTest, EncryptedFieldsConflict, "tripwire assertions") {
    EncryptedField a(UUID::gen(), "a");
    a.setBsonType("string"_sd);
    EncryptedField ab(UUID::gen(), "a.b");
    ab.setBsonType("int"_sd);
    EncryptedField abc(UUID::gen(), "a.b.c");
    abc.setBsonType("int"_sd);

    auto swExpr =
        generateMatchExpressionFromEncryptedFields(new ExpressionContextForTest(), {a, ab});
    ASSERT(!swExpr.isOK());
    ASSERT(swExpr.getStatus().code() == 6364302);

    swExpr = generateMatchExpressionFromEncryptedFields(new ExpressionContextForTest(), {abc, ab});
    ASSERT(!swExpr.isOK());
    ASSERT(swExpr.getStatus().code() == 6364302);

    swExpr = generateMatchExpressionFromEncryptedFields(new ExpressionContextForTest(), {abc, abc});
    ASSERT(!swExpr.isOK());
    ASSERT(swExpr.getStatus().code() == 6364302);
}

class Fle2MatchTest : public GenerateFLE2MatchExpression {
public:
    Fle2MatchTest() {
        expr = uassertStatusOK(generateMatchExpressionFromEncryptedFields(
            new ExpressionContextForTest(), kEncryptedFields));
    }
    std::unique_ptr<MatchExpression> expr;
};

TEST_F(Fle2MatchTest, MatchesIfNoEncryptedFieldsInObject) {
    // no encrypted paths
    ASSERT(exec::matcher::matchesBSON(expr.get(), BSONObj()));
    ASSERT(exec::matcher::matchesBSON(expr.get(), fromjson(R"({name: "sue"})")));

    // has prefix of encrypted paths, but no leaf
    ASSERT(exec::matcher::matchesBSON(expr.get(), fromjson(R"({a: {}})")));
    ASSERT(exec::matcher::matchesBSON(expr.get(), fromjson(R"({a: {b: {}, x: { count: 23 }}})")));

    // non-object/non-array along the encrypted path
    ASSERT(exec::matcher::matchesBSON(expr.get(), fromjson(R"({a: 1})")));
    ASSERT(exec::matcher::matchesBSON(expr.get(), fromjson(R"({a: { b: 2, x: "foo"}})")));
}

TEST_F(Fle2MatchTest, MatchesIfSomeEncryptedFieldsInObject) {
    auto obj = BSON("c" << makeFleBinData(kValueC) << "other"
                        << "foo");
    ASSERT(exec::matcher::matchesBSON(expr.get(), obj));

    obj = BSON("a" << BSON("b" << BSON("c" << makeFleBinData(kValueAbc))));
    ASSERT(exec::matcher::matchesBSON(expr.get(), obj));
}

TEST_F(Fle2MatchTest, MatchesIfAllEncryptedFieldsInObject) {
    auto allIn = BSON("c" << makeFleBinData(kValueC) << "a"
                          << BSON("b" << BSON("c" << makeFleBinData(kValueAbc) << "d"
                                                  << makeFleBinData(kValueAbd))
                                      << "x" << BSON("y" << makeFleBinData(kValueAxy))));
    ASSERT(exec::matcher::matchesBSON(expr.get(), allIn));
}

TEST_F(Fle2MatchTest, DoesNotMatchIfEncryptedFieldIsNotBinDataEncrypt) {
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.get(), fromjson(R"({a: {b: {c: "foo"}}})")));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.get(), fromjson(R"({c: []})")));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.get(), fromjson(R"({a: {x: {y: [1, 2, 3]}}})")));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.get(), fromjson(R"({a: {b: {d: 42}}})")));
    auto obj = BSON("c" << BSONBinData(nullptr, 0, BinDataType::BinDataGeneral));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.get(), obj));
}

TEST_F(Fle2MatchTest, DoesNotMatchIfEncryptedFieldIsNotFLE2) {
    auto obj = BSON("c" << makeFleBinData(kValueFLE1));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.get(), obj));

    obj = BSON("a" << BSON("b" << BSON("c" << makeFleBinData(kValueFLE1))));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.get(), obj));
}

TEST_F(Fle2MatchTest, DoesNotMatchIfTypeMismatch) {
    auto obj = BSON("c" << makeFleBinData(kValueAbc));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.get(), obj));

    obj = BSON("a" << BSON_ARRAY(BSON("b" << BSON("c" << makeFleBinData(kValueAxy)))));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.get(), obj));
}

TEST_F(Fle2MatchTest, DoesNotMatchIfHasArrayInEncryptedFieldPath) {
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.get(), fromjson(R"({a: []})")));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.get(), fromjson(R"({a: {b: [1, 2, 3]}})")));

    auto obj = BSON("a" << BSON_ARRAY(BSON("b" << BSON("c" << makeFleBinData(kValueAbc)))));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.get(), obj));
}

TEST_F(Fle2MatchTest, MatchOptionalType) {
    // Match against indexed
    auto obj = BSON("a" << BSON("x" << BSON("t" << makeFleBinData(kValueAxy))));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.get(), obj));

    // Match against unindexed
    auto obj2 = BSON("a" << BSON("x" << BSON("t" << makeFleBinData(kValueAxt))));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.get(), obj2));
}

}  // namespace
}  // namespace mongo
