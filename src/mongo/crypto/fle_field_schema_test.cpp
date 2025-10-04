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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

TEST(FLE2EncryptionPlaceholder, RangeScalarAsValueFails) {
    FLE2EncryptionPlaceholder p;
    BSONObj value = BSON("" << 6);
    p.setAlgorithm(Fle2AlgorithmInt::kRange);
    p.setIndexKeyId(UUID::gen());
    p.setUserKeyId(p.getIndexKeyId());
    p.setValue(value.firstElement());
    p.setMaxContentionCounter(9);
    p.setSparsity(1);

    p.setType(Fle2PlaceholderType::kInsert);
    ASSERT_THROWS_CODE(
        FLE2EncryptionPlaceholder::parse(p.toBSON(), IDLParserContext("placeholder")),
        DBException,
        6775321);
    p.setType(Fle2PlaceholderType::kFind);
    ASSERT_THROWS_CODE(
        FLE2EncryptionPlaceholder::parse(p.toBSON(), IDLParserContext("placeholder")),
        DBException,
        6720200);
}

TEST(FLE2EncryptionPlaceholder, RangeBadObjectAsValueFails) {
    FLE2EncryptionPlaceholder p;
    BSONObj value = BSON("" << BSON("foo" << 2));
    p.setAlgorithm(Fle2AlgorithmInt::kTextSearch);
    p.setIndexKeyId(UUID::gen());
    p.setUserKeyId(p.getIndexKeyId());
    p.setValue(value.firstElement());
    p.setMaxContentionCounter(9);
    p.setSparsity(1);
    for (auto type : {Fle2PlaceholderType::kInsert, Fle2PlaceholderType::kFind}) {
        p.setType(type);
        ASSERT_THROWS_CODE(
            FLE2EncryptionPlaceholder::parse(p.toBSON(), IDLParserContext("placeholder")),
            DBException,
            ErrorCodes::IDLUnknownField);
    }
}

TEST(FLE2EncryptionPlaceholder, RangeMissingSparsity) {
    FLE2EncryptionPlaceholder p;
    auto insertSpecBackingBSON = BSON("" << BSON("v" << int32_t(2)));
    auto findSpecBackingBSON =
        BSON("" << BSON("payloadId" << int32_t(1) << "firstOperator" << int32_t(1)));
    p.setAlgorithm(Fle2AlgorithmInt::kRange);
    p.setIndexKeyId(UUID::gen());
    p.setUserKeyId(p.getIndexKeyId());
    p.setMaxContentionCounter(9);

    p.setType(Fle2PlaceholderType::kInsert);
    p.setValue(insertSpecBackingBSON.firstElement());
    ASSERT_THROWS_CODE(
        FLE2EncryptionPlaceholder::parse(p.toBSON(), IDLParserContext("placeholder")),
        DBException,
        6775322);

    p.setType(Fle2PlaceholderType::kFind);
    p.setValue(findSpecBackingBSON.firstElement());
    ASSERT_THROWS_CODE(
        FLE2EncryptionPlaceholder::parse(p.toBSON(), IDLParserContext("placeholder")),
        DBException,
        6832501);
}

TEST(FLE2EncryptionPlaceholder, NonRangeAlgorithmWithSparsity) {
    FLE2EncryptionPlaceholder p;
    BSONObj value = BSON("foo" << "bar");
    p.setType(Fle2PlaceholderType::kInsert);
    p.setIndexKeyId(UUID::gen());
    p.setUserKeyId(p.getIndexKeyId());
    p.setValue(value.firstElement());
    p.setMaxContentionCounter(9);
    p.setSparsity(2);

    for (auto alg : {Fle2AlgorithmInt::kUnindexed, Fle2AlgorithmInt::kEquality}) {
        p.setAlgorithm(alg);
        ASSERT_THROWS_CODE(
            FLE2EncryptionPlaceholder::parse(p.toBSON(), IDLParserContext("placeholder")),
            DBException,
            6832500);
    }

    // text search case
    {
        p.setAlgorithm(Fle2AlgorithmInt::kTextSearch);
        FLE2TextSearchInsertSpec spec("foo", false, true);
        spec.setSubstringSpec(FLE2SubstringInsertSpec(100, 10, 1));
        auto backingBSON = BSON("" << spec.toBSON());
        p.setValue(backingBSON.firstElement());
        ASSERT_THROWS_CODE(
            FLE2EncryptionPlaceholder::parse(p.toBSON(), IDLParserContext("placeholder")),
            DBException,
            6832500);
    }
}

TEST(FLE2EncryptionPlaceholder, TextSearchRoundTrip) {
    FLE2TextSearchInsertSpec spec("foo", false /*casefold*/, true /*diacriticfold*/);
    spec.setSuffixSpec(FLE2SuffixInsertSpec(10, 1));
    spec.setPrefixSpec(FLE2PrefixInsertSpec(20, 2));
    spec.setSubstringSpec(FLE2SubstringInsertSpec(300, 30, 3));
    auto backingBSON = BSON("" << spec.toBSON());

    FLE2EncryptionPlaceholder p;
    p.setType(Fle2PlaceholderType::kInsert);
    p.setAlgorithm(Fle2AlgorithmInt::kTextSearch);
    p.setIndexKeyId(UUID::gen());
    p.setUserKeyId(p.getIndexKeyId());
    p.setValue(backingBSON.firstElement());
    p.setMaxContentionCounter(9);
    auto serialized = p.toBSON();

    auto parsedPlaceholder =
        FLE2EncryptionPlaceholder::parse(serialized, IDLParserContext("placeholder"));
    auto parsedSpec = FLE2TextSearchInsertSpec::parse(
        parsedPlaceholder.getValue().getElement().Obj(), IDLParserContext("text"));

    ASSERT_TRUE(parsedSpec.getSubstringSpec());
    ASSERT_TRUE(parsedSpec.getSuffixSpec());
    ASSERT_TRUE(parsedSpec.getPrefixSpec());
    ASSERT_FALSE(parsedSpec.getCaseFold());
    ASSERT_TRUE(parsedSpec.getDiacriticFold());

    auto& suffixSpec = parsedSpec.getSuffixSpec().value();
    ASSERT_EQ(suffixSpec.getMaxQueryLength(), 10);
    ASSERT_EQ(suffixSpec.getMinQueryLength(), 1);
    auto& prefixSpec = parsedSpec.getPrefixSpec().value();
    ASSERT_EQ(prefixSpec.getMaxQueryLength(), 20);
    ASSERT_EQ(prefixSpec.getMinQueryLength(), 2);
    auto& substrSpec = parsedSpec.getSubstringSpec().value();
    ASSERT_EQ(substrSpec.getMaxLength(), 300);
    ASSERT_EQ(substrSpec.getMaxQueryLength(), 30);
    ASSERT_EQ(substrSpec.getMinQueryLength(), 3);
}

TEST(FLE2EncryptionPlaceholder, TextSearchScalarAsValueFails) {
    FLE2EncryptionPlaceholder p;
    BSONObj value = BSON("" << 6);
    p.setAlgorithm(Fle2AlgorithmInt::kTextSearch);
    p.setIndexKeyId(UUID::gen());
    p.setUserKeyId(p.getIndexKeyId());
    p.setValue(value.firstElement());
    p.setMaxContentionCounter(9);

    for (auto type : {Fle2PlaceholderType::kInsert, Fle2PlaceholderType::kFind}) {
        p.setType(type);
        ASSERT_THROWS_CODE(
            FLE2EncryptionPlaceholder::parse(p.toBSON(), IDLParserContext("placeholder")),
            DBException,
            9783505);
    }
}

TEST(FLE2EncryptionPlaceholder, TextSearchBadObjectAsValueFails) {
    FLE2EncryptionPlaceholder p;
    BSONObj value = BSON("" << BSON("foo" << 2));
    p.setAlgorithm(Fle2AlgorithmInt::kTextSearch);
    p.setIndexKeyId(UUID::gen());
    p.setUserKeyId(p.getIndexKeyId());
    p.setValue(value.firstElement());
    p.setMaxContentionCounter(9);
    for (auto type : {Fle2PlaceholderType::kInsert, Fle2PlaceholderType::kFind}) {
        p.setType(type);
        ASSERT_THROWS_CODE(
            FLE2EncryptionPlaceholder::parse(p.toBSON(), IDLParserContext("placeholder")),
            DBException,
            ErrorCodes::IDLUnknownField);
    }
}

TEST(FLE2RangeFindSpec, UpperAndLowerBoundTypeMismatches) {
    BSONObj values =
        BSON("int" << int32_t(2) << "smallLong" << int64_t(48) << "largeLong" << int64_t(2147483650)
                   << "date" << Date_t() << "double" << 1.2 << "decimal" << Decimal128()
                   << "infinity" << std::numeric_limits<double>::infinity());

    auto intValue = values.getField("int");
    auto smallLongValue = values.getField("smallLong");
    auto largeLongValue = values.getField("largeLong");
    auto dateValue = values.getField("date");
    auto doubleValue = values.getField("double");
    auto decimalValue = values.getField("decimal");
    auto infiniteValue = values.getField("infinity");

    auto doParseTest = [](BSONElement value, BSONElement lbValue, BSONElement ubValue) {
        FLE2RangeFindSpec spec{0, mongo::Fle2RangeOperator::kGt};
        FLE2RangeFindSpecEdgesInfo ei;
        ei.setLowerBound(lbValue);
        ei.setLbIncluded(true);
        ei.setUpperBound(ubValue);
        ei.setUbIncluded(false);
        ei.setIndexMax(value);
        ei.setIndexMin(value);
        spec.setEdgesInfo(std::move(ei));

        return FLE2RangeFindSpec::parse(spec.toBSON(), IDLParserContext("FLE2RangeFindSpec"));
    };

    for (auto& badValue : {intValue, largeLongValue, dateValue, doubleValue, decimalValue}) {

        if (badValue.type() != intValue.type()) {
            ASSERT_THROWS_CODE(doParseTest(intValue, badValue, intValue), DBException, 6901306);
            ASSERT_THROWS_CODE(doParseTest(intValue, intValue, badValue), DBException, 6901307);
        }
        if (badValue.type() != largeLongValue.type() && badValue.type() != intValue.type()) {
            ASSERT_THROWS_CODE(
                doParseTest(largeLongValue, badValue, largeLongValue), DBException, 6901308);
            ASSERT_THROWS_CODE(
                doParseTest(largeLongValue, largeLongValue, badValue), DBException, 6901309);
        }
        if (badValue.type() != dateValue.type()) {
            ASSERT_THROWS_CODE(doParseTest(dateValue, badValue, dateValue), DBException, 6901310);
            ASSERT_THROWS_CODE(doParseTest(dateValue, dateValue, badValue), DBException, 6901311);
        }
        if (badValue.type() != doubleValue.type()) {
            ASSERT_THROWS_CODE(
                doParseTest(doubleValue, badValue, doubleValue), DBException, 6901312);
            ASSERT_THROWS_CODE(
                doParseTest(doubleValue, doubleValue, badValue), DBException, 6901313);
        }
        if (badValue.type() != decimalValue.type()) {
            ASSERT_THROWS_CODE(
                doParseTest(decimalValue, badValue, decimalValue), DBException, 6901314);
            ASSERT_THROWS_CODE(
                doParseTest(decimalValue, decimalValue, badValue), DBException, 6901315);
        }

        ASSERT_DOES_NOT_THROW(doParseTest(badValue, badValue, badValue));
        ASSERT_DOES_NOT_THROW(doParseTest(badValue, infiniteValue, badValue));
        ASSERT_DOES_NOT_THROW(doParseTest(badValue, badValue, infiniteValue));
    }

    // special case: ints are ok for lb and ub if value is long
    ASSERT_DOES_NOT_THROW(doParseTest(smallLongValue, intValue, intValue));
    // special case: small long values are ok for lb and ub if value is int
    ASSERT_DOES_NOT_THROW(doParseTest(intValue, smallLongValue, smallLongValue));
}

TEST(FLE2RangeFindSpec, MinMaxTypeMismatch) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder subBob{bob.subobjStart("edgesInfo")};
        subBob.append("lowerBound", int32_t(2));
        subBob.append("upperBound", int32_t(2));
        subBob.append("lbIncluded", false);
        subBob.append("ubIncluded", true);
        subBob.append("indexMin", int32_t(1));
        subBob.append("indexMax", int64_t(4));
    }
    bob.append("payloadId", int32_t(1));
    bob.append("firstOperator", int32_t(1));
    ASSERT_THROWS_CODE(
        FLE2RangeFindSpec::parse(bob.asTempObj(), IDLParserContext("FLE2RangeFindSpec")),
        DBException,
        6901304);
}

TEST(FLE2RangeFindSpec, InvalidTrimFactor) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder subBob{bob.subobjStart("edgesInfo")};
        subBob.append("lowerBound", int32_t(2));
        subBob.append("upperBound", int32_t(2));
        subBob.append("lbIncluded", false);
        subBob.append("ubIncluded", true);
        subBob.append("indexMin", int32_t(1));
        subBob.append("indexMax", int32_t(4));
        subBob.append("trimFactor", 1000);
    }
    bob.append("payloadId", int32_t(1));
    bob.append("firstOperator", int32_t(1));
    ASSERT_THROWS_CODE(
        FLE2RangeFindSpec::parse(bob.asTempObj(), IDLParserContext("FLE2RangeFindSpec")),
        DBException,
        8574100);
}

TEST(FLE2RangeFindSpec, PrecisionApplicability) {
    BSONObjBuilder bob;
    BSONObj values = BSON("int" << int32_t(2) << "long" << int64_t(3) << "date" << Date_t()
                                << "double" << 1.2 << "decimal" << Decimal128());
    auto buildTestSpec = [](BSONElement value) {
        FLE2RangeFindSpec spec{0, mongo::Fle2RangeOperator::kGt};
        FLE2RangeFindSpecEdgesInfo ei;
        ei.setLowerBound(value);
        ei.setLbIncluded(true);
        ei.setUpperBound(value);
        ei.setUbIncluded(false);
        ei.setIndexMax(value);
        ei.setIndexMin(value);
        ei.setPrecision(5);
        spec.setEdgesInfo(std::move(ei));
        return spec;
    };

    for (auto& type : {"int", "long", "date"}) {
        auto spec = buildTestSpec(values.getField(type));
        ASSERT_THROWS_CODE(
            FLE2RangeFindSpec::parse(spec.toBSON(), IDLParserContext("FLE2RangeFindSpec")),
            DBException,
            6967102);
    }

    for (auto& type : {"double", "decimal"}) {
        auto spec = buildTestSpec(values.getField(type));
        ASSERT_DOES_NOT_THROW(
            FLE2RangeFindSpec::parse(spec.toBSON(), IDLParserContext("FLE2RangeFindSpec")));
    }
}

TEST(FLE2RangeInsertSpec, ValueNotNumeric) {
    BSONObjBuilder bob;
    bob.append("v", "foo");
    bob.append("min", int32_t(1));
    bob.append("max", int32_t(4));
    ASSERT_THROWS_CODE(
        FLE2RangeInsertSpec::parse(bob.asTempObj(), IDLParserContext("FLE2RangeInsertSpec")),
        DBException,
        ErrorCodes::TypeMismatch);
}

TEST(FLE2RangeInsertSpec, MinMaxTypeMismatch) {
    BSONObjBuilder bob;
    bob.append("v", int32_t(23));
    bob.append("min", int32_t(23));
    bob.append("max", int64_t(64));
    ASSERT_THROWS_CODE(
        FLE2RangeInsertSpec::parse(bob.asTempObj(), IDLParserContext("FLE2RangeInsertSpec")),
        DBException,
        8574101);
}

TEST(FLE2RangeInsertSpec, ValueMinTypeMismatch) {
    BSONObjBuilder bob;
    bob.append("v", int64_t(23));
    bob.append("min", int32_t(23));
    bob.append("max", int32_t(64));
    ASSERT_THROWS_CODE(
        FLE2RangeInsertSpec::parse(bob.asTempObj(), IDLParserContext("FLE2RangeInsertSpec")),
        DBException,
        8574109);
}

TEST(FLE2RangeInsertSpec, InvalidTrimFactor) {
    BSONObjBuilder bob;
    bob.append("v", int32_t(2));
    bob.append("min", int32_t(1));
    bob.append("max", int32_t(4));
    bob.append("trimFactor", 1000);
    ASSERT_THROWS_CODE(
        FLE2RangeInsertSpec::parse(bob.asTempObj(), IDLParserContext("FLE2RangeInsertSpec")),
        DBException,
        8574103);
}

TEST(FLE2RangeInsertSpec, PrecisionApplicability) {
    BSONObjBuilder bob;
    BSONObj values = BSON("int" << int32_t(2) << "long" << int64_t(3) << "date" << Date_t()
                                << "double" << 1.2 << "decimal" << Decimal128());

    for (auto& type : {"int", "long", "date"}) {
        FLE2RangeInsertSpec spec;
        spec.setValue(values.getField(type));
        spec.setPrecision(5);
        ASSERT_THROWS_CODE(
            FLE2RangeInsertSpec::parse(spec.toBSON(), IDLParserContext("FLE2RangeInsertSpec")),
            DBException,
            8574102);
    }

    for (auto& type : {"double", "decimal"}) {
        FLE2RangeInsertSpec spec;
        spec.setValue(values.getField(type));
        spec.setPrecision(5);
        ASSERT_DOES_NOT_THROW(
            FLE2RangeInsertSpec::parse(spec.toBSON(), IDLParserContext("FLE2RangeInsertSpec")));
    }
}

TEST(FLE2TextSearchInsertSpec, MissingSubspec) {
    FLE2TextSearchInsertSpec spec("foo", false /*casefold*/, true /*diacriticfold*/);
    ASSERT_THROWS_CODE(FLE2TextSearchInsertSpec::parse(
                           spec.toBSON(), IDLParserContext("FLE2TextSearchInsertSpec")),
                       DBException,
                       9783500);
}

TEST(FLE2TextSearchInsertSpec, SubstringSpecUpperBoundLessThanLowerBound) {
    FLE2TextSearchInsertSpec spec("foo", false /*casefold*/, true /*diacriticfold*/);
    spec.setSubstringSpec(FLE2SubstringInsertSpec(100, 1 /*ub*/, 10 /*lb*/));
    ASSERT_THROWS_CODE(FLE2TextSearchInsertSpec::parse(
                           spec.toBSON(), IDLParserContext("FLE2TextSearchInsertSpec")),
                       DBException,
                       9783501);
}

TEST(FLE2TextSearchInsertSpec, SubstringSpecUpperBoundGreaterThanMaxLen) {
    FLE2TextSearchInsertSpec spec("foo", false /*casefold*/, true /*diacriticfold*/);
    spec.setSubstringSpec(FLE2SubstringInsertSpec(10 /*mlen*/, 100 /*ub*/, 1 /*lb*/));
    ASSERT_THROWS_CODE(FLE2TextSearchInsertSpec::parse(
                           spec.toBSON(), IDLParserContext("FLE2TextSearchInsertSpec")),
                       DBException,
                       9783502);
}

TEST(FLE2TextSearchInsertSpec, SuffixSpecUpperBoundLessThanLowerBound) {
    FLE2TextSearchInsertSpec spec("foo", false /*casefold*/, true /*diacriticfold*/);
    spec.setSuffixSpec(FLE2SuffixInsertSpec(1 /*ub*/, 10 /*lb*/));
    ASSERT_THROWS_CODE(FLE2TextSearchInsertSpec::parse(
                           spec.toBSON(), IDLParserContext("FLE2TextSearchInsertSpec")),
                       DBException,
                       9783503);
}

TEST(FLE2TextSearchInsertSpec, PrefixSpecUpperBoundLessThanLowerBound) {
    FLE2TextSearchInsertSpec spec("foo", false /*casefold*/, true /*diacriticfold*/);
    spec.setPrefixSpec(FLE2PrefixInsertSpec(1 /*ub*/, 10 /*lb*/));
    ASSERT_THROWS_CODE(FLE2TextSearchInsertSpec::parse(
                           spec.toBSON(), IDLParserContext("FLE2TextSearchInsertSpec")),
                       DBException,
                       9783504);
}

TEST(FLE2FindTextPayload, EmptyTokenSetsObject) {
    FLE2FindTextPayload payload;
    payload.setCaseFold(false);
    payload.setDiacriticFold(false);
    payload.setMaxCounter(22);
    payload.setTokenSets(mongo::TextSearchFindTokenSets{});
    ASSERT_THROWS_CODE(
        FLE2FindTextPayload::parse(payload.toBSON(), IDLParserContext{"FLE2FindTextPayload"}),
        DBException,
        10163701);
}

TEST(FLE2FindTextPayload, MultipleTokenSets) {
    FLE2FindTextPayload payload;
    PrfBlock tokenData;

    payload.setCaseFold(false);
    payload.setDiacriticFold(false);
    payload.setMaxCounter(22);
    payload.setTokenSets(mongo::TextSearchFindTokenSets{});
    payload.getTokenSets().setSubstringTokens(
        TextSubstringFindTokenSet{EDCTextSubstringDerivedFromDataToken{tokenData},
                                  ESCTextSubstringDerivedFromDataToken{tokenData},
                                  ServerTextSubstringDerivedFromDataToken{tokenData}});
    payload.getTokenSets().setPrefixTokens(
        TextPrefixFindTokenSet{EDCTextPrefixDerivedFromDataToken{tokenData},
                               ESCTextPrefixDerivedFromDataToken{tokenData},
                               ServerTextPrefixDerivedFromDataToken{tokenData}});
    ASSERT_THROWS_CODE(
        FLE2FindTextPayload::parse(payload.toBSON(), IDLParserContext{"FLE2FindTextPayload"}),
        DBException,
        10163701);
}

}  // namespace mongo
