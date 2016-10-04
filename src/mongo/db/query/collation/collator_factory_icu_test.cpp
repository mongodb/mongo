/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/query/collation/collator_factory_icu.h"

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

TEST(CollatorFactoryICUTest, LocaleStringParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"));
    ASSERT_OK(collator.getStatus());
    ASSERT_EQ("en_US", collator.getValue()->getSpec().localeID);
}

TEST(CollatorFactoryICUTest, SimpleLocaleReturnsNullPointer) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "simple"));
    ASSERT_OK(collator.getStatus());
    ASSERT_TRUE(collator.getValue() == nullptr);
}

TEST(CollatorFactoryICUTest, SimpleLocaleWithOtherFieldsFailsToParse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "simple"
                                              << "caseLevel"
                                              << true));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::FailedToParse);
}

TEST(CollatorFactoryICUTest, LocaleFieldNotAStringFailsToParse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale" << 3));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(CollatorFactoryICUTest, UnrecognizedLocaleFieldFailsToParse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "garbage"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleEmptyStringDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << ""));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LongLocaleFieldDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LongLocaleFieldWithKeywordsDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US@xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx=y"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleRootDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "root"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleRootCanonicalizedDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "ROOT@collation=search"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithOneComponentAndTrailingUnderscoreDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithTwoComponentsAndTrailingUnderscoreDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US_"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithTwoTrailingUnderscoresDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en__"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}


TEST(CollatorFactoryICUTest, LocaleWithLeadingUnderscoreDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "_en"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithWithTwoComponentsAndLeadingUnderscoreDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "_en_US"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithTwoLeadingUnderscoresDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "__en"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithExtraEmptyComponentDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en__US_POSIX"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithExtraNonEmptyComponentDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_Comp_US_POSIX"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithUnrecognizedTwoLetterBaseNameDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "xx_US"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithUnrecognizedTwoLetterCountryCodeDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_ZZ"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithInvalidThreeLetterCountryCodeDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_USX"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithValidThreeLetterCountryCodeDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_USA"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithUnrecognizedFourLetterScriptCodeDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "zh_Blah"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithFiveLetterSecondComponentDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "zh_Blahh"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithUnrecognizedVariantCode) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US_FOO"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithMissingCountryCodeAndUnrecognizedVariantCode) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en__FOO"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithValidVariantButMissingCountryCodeDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en__POSIX"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithValidKeywordButNoLanguageDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "@collation=phonebook"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithHyphenSeparatingLanguageAndCountryDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en-US"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithHyphenSeparatingCountryAndVariantDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US-POSIX"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithLowercaseCountryCodeDisallwed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_us"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithMixedCaseCountryCodeDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_uS"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithLowercaseCountryCodeAndVariantDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_us_posix"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithUpperCaseScriptCodeDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "zh_HANT"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithUpperCaseLanguageCodeDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "ZH_hant"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithKeywordStringWithoutAValueDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "de@collation"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleKeywordStringWithExcessTrailingSemicolonDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "de@collation=phonebook;"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleKeywordStringWithExcessLeadingSemicolonDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "de@;collation=phonebook"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, TaiwanLocaleWithCollationStrokeDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "zh_TW@collation=stroke"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, LocaleWithValidLanguageCountryAndVariantAllowed) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "en_US_POSIX"))
                  .getStatus());
}

TEST(CollatorFactoryICUTest, USLocaleWithCollationPhonebookDisallowed) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US@collation=phonebook"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, GermanLocaleWithCollationPhonebookAllowed) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "de@collation=phonebook"))
                  .getStatus());
}

TEST(CollatorFactoryICUTest, ChineseTraditionalLocaleWithCollationPinyinAllowed) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "zh_Hant@collation=pinyin"))
                  .getStatus());
}

TEST(CollatorFactoryICUTest, LocaleStringCannotContainNullByte) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US\0"_sd));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, MissingLocaleStringFailsToParse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSONObj());
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::NoSuchKey);
}

TEST(CollatorFactoryICUTest, UnknownSpecFieldFailsToParse) {
    BSONObj spec = BSON("locale"
                        << "en_US"
                        << "unknown"
                        << "field");
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(spec);
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::FailedToParse);
}

TEST(CollatorFactoryICUTest, DefaultsSetSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"));
    ASSERT_OK(collator.getStatus());
    ASSERT_FALSE(collator.getValue()->getSpec().caseLevel);
    ASSERT_EQ(static_cast<int>(CollationSpec::CaseFirstType::kOff),
              static_cast<int>(collator.getValue()->getSpec().caseFirst));
    ASSERT_EQ(static_cast<int>(CollationSpec::StrengthType::kTertiary),
              static_cast<int>(collator.getValue()->getSpec().strength));
    ASSERT_FALSE(collator.getValue()->getSpec().numericOrdering);
    ASSERT_EQ(static_cast<int>(CollationSpec::AlternateType::kNonIgnorable),
              static_cast<int>(collator.getValue()->getSpec().alternate));
    ASSERT_EQ(static_cast<int>(CollationSpec::MaxVariableType::kPunct),
              static_cast<int>(collator.getValue()->getSpec().maxVariable));
    ASSERT_FALSE(collator.getValue()->getSpec().normalization);
    ASSERT_FALSE(collator.getValue()->getSpec().backwards);
}

TEST(CollatorFactoryICUTest, LanguageDependentDefaultsSetSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "fr_CA"));
    ASSERT_OK(collator.getStatus());
    ASSERT_TRUE(collator.getValue()->getSpec().backwards);
}

TEST(CollatorFactoryICUTest, CaseLevelFalseParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "caseLevel"
                                              << false));
    ASSERT_OK(collator.getStatus());
    ASSERT_FALSE(collator.getValue()->getSpec().caseLevel);
}

TEST(CollatorFactoryICUTest, CaseLevelTrueParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "caseLevel"
                                              << true));
    ASSERT_OK(collator.getStatus());
    ASSERT_TRUE(collator.getValue()->getSpec().caseLevel);
}

TEST(CollatorFactoryICUTest, CaseFirstOffParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "caseFirst"
                                              << "off"));
    ASSERT_OK(collator.getStatus());
    ASSERT_EQ(static_cast<int>(CollationSpec::CaseFirstType::kOff),
              static_cast<int>(collator.getValue()->getSpec().caseFirst));
}

TEST(CollatorFactoryICUTest, CaseFirstUpperParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "caseFirst"
                                              << "upper"));
    ASSERT_OK(collator.getStatus());
    ASSERT_EQ(static_cast<int>(CollationSpec::CaseFirstType::kUpper),
              static_cast<int>(collator.getValue()->getSpec().caseFirst));
}

TEST(CollatorFactoryICUTest, CaseFirstLowerParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "caseFirst"
                                              << "lower"));
    ASSERT_OK(collator.getStatus());
    ASSERT_EQ(static_cast<int>(CollationSpec::CaseFirstType::kLower),
              static_cast<int>(collator.getValue()->getSpec().caseFirst));
}

TEST(CollatorFactoryICUTest, PrimaryStrengthParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 1));
    ASSERT_OK(collator.getStatus());
    ASSERT_EQ(static_cast<int>(CollationSpec::StrengthType::kPrimary),
              static_cast<int>(collator.getValue()->getSpec().strength));
}

TEST(CollatorFactoryICUTest, SecondaryStrengthParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 2));
    ASSERT_OK(collator.getStatus());
    ASSERT_EQ(static_cast<int>(CollationSpec::StrengthType::kSecondary),
              static_cast<int>(collator.getValue()->getSpec().strength));
}

TEST(CollatorFactoryICUTest, TertiaryStrengthParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 3));
    ASSERT_OK(collator.getStatus());
    ASSERT_EQ(static_cast<int>(CollationSpec::StrengthType::kTertiary),
              static_cast<int>(collator.getValue()->getSpec().strength));
}

TEST(CollatorFactoryICUTest, QuaternaryStrengthParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 4));
    ASSERT_OK(collator.getStatus());
    ASSERT_EQ(static_cast<int>(CollationSpec::StrengthType::kQuaternary),
              static_cast<int>(collator.getValue()->getSpec().strength));
}

TEST(CollatorFactoryICUTest, IdenticalStrengthParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 5));
    ASSERT_OK(collator.getStatus());
    ASSERT_EQ(static_cast<int>(CollationSpec::StrengthType::kIdentical),
              static_cast<int>(collator.getValue()->getSpec().strength));
}

TEST(CollatorFactoryICUTest, NumericOrderingFalseParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "numericOrdering"
                                              << false));
    ASSERT_OK(collator.getStatus());
    ASSERT_FALSE(collator.getValue()->getSpec().numericOrdering);
}

TEST(CollatorFactoryICUTest, NumericOrderingTrueParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "numericOrdering"
                                              << true));
    ASSERT_OK(collator.getStatus());
    ASSERT_TRUE(collator.getValue()->getSpec().numericOrdering);
}

TEST(CollatorFactoryICUTest, AlternateNonIgnorableParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "alternate"
                                              << "non-ignorable"));
    ASSERT_OK(collator.getStatus());
    ASSERT_EQ(static_cast<int>(CollationSpec::AlternateType::kNonIgnorable),
              static_cast<int>(collator.getValue()->getSpec().alternate));
}

TEST(CollatorFactoryICUTest, AlternateShiftedParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "alternate"
                                              << "shifted"));
    ASSERT_OK(collator.getStatus());
    ASSERT_EQ(static_cast<int>(CollationSpec::AlternateType::kShifted),
              static_cast<int>(collator.getValue()->getSpec().alternate));
}

TEST(CollatorFactoryICUTest, MaxVariablePunctParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "maxVariable"
                                              << "punct"));
    ASSERT_OK(collator.getStatus());
    ASSERT_EQ(static_cast<int>(CollationSpec::MaxVariableType::kPunct),
              static_cast<int>(collator.getValue()->getSpec().maxVariable));
}

TEST(CollatorFactoryICUTest, MaxVariableSpaceParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "maxVariable"
                                              << "space"));
    ASSERT_OK(collator.getStatus());
    ASSERT_EQ(static_cast<int>(CollationSpec::MaxVariableType::kSpace),
              static_cast<int>(collator.getValue()->getSpec().maxVariable));
}

TEST(CollatorFactoryICUTest, NormalizationFalseParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "normalization"
                                              << false));
    ASSERT_OK(collator.getStatus());
    ASSERT_FALSE(collator.getValue()->getSpec().normalization);
}

TEST(CollatorFactoryICUTest, NormalizationTrueParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "normalization"
                                              << true));
    ASSERT_OK(collator.getStatus());
    ASSERT_TRUE(collator.getValue()->getSpec().normalization);
}

TEST(CollatorFactoryICUTest, BackwardsFalseParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "backwards"
                                              << false));
    ASSERT_OK(collator.getStatus());
    ASSERT_FALSE(collator.getValue()->getSpec().backwards);
}

TEST(CollatorFactoryICUTest, BackwardsTrueParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "backwards"
                                              << true));
    ASSERT_OK(collator.getStatus());
    ASSERT_TRUE(collator.getValue()->getSpec().backwards);
}

TEST(CollatorFactoryICUTest, LongStrengthFieldParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 1LL));
    ASSERT_OK(collator.getStatus());
    ASSERT_EQ(static_cast<int>(CollationSpec::StrengthType::kPrimary),
              static_cast<int>(collator.getValue()->getSpec().strength));
}

TEST(CollatorFactoryICUTest, DoubleStrengthFieldParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 1.0));
    ASSERT_OK(collator.getStatus());
    ASSERT_EQ(static_cast<int>(CollationSpec::StrengthType::kPrimary),
              static_cast<int>(collator.getValue()->getSpec().strength));
}

TEST(CollatorFactoryICUTest, NonBooleanCaseLevelFieldFailsToParse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "caseLevel"
                                              << "garbage"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(CollatorFactoryICUTest, NonStringCaseFirstFieldFailsToParse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "caseFirst"
                                              << 1));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(CollatorFactoryICUTest, InvalidStringCaseFirstFieldFailsToParse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "caseFirst"
                                              << "invalid"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::FailedToParse);
}

TEST(CollatorFactoryICUTest, NonNumberStrengthFieldFailsToParse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << "garbage"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(CollatorFactoryICUTest, TooLargeStrengthFieldFailsToParse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 2147483648LL));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::FailedToParse);
}

TEST(CollatorFactoryICUTest, FractionalStrengthFieldFailsToParse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 0.5));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::BadValue);
}

TEST(CollatorFactoryICUTest, NegativeStrengthFieldFailsToParse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << -1));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::FailedToParse);
}

TEST(CollatorFactoryICUTest, InvalidIntegerStrengthFieldFailsToParse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 6));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::FailedToParse);
}

TEST(CollatorFactoryICUTest, NonBoolNumericOrderingFieldFailsToParse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "numericOrdering"
                                              << "garbage"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(CollatorFactoryICUTest, NonStringAlternateFieldFailsToParse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "alternate"
                                              << 1));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(CollatorFactoryICUTest, InvalidStringAlternateFieldFailsToParse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "alternate"
                                              << "invalid"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::FailedToParse);
}

TEST(CollatorFactoryICUTest, NonStringMaxVariableFieldFailsToParse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "maxVariable"
                                              << 1));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(CollatorFactoryICUTest, InvalidStringMaxVariableFieldFailsToParse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "maxVariable"
                                              << "invalid"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::FailedToParse);
}

TEST(CollatorFactoryICUTest, NonBoolNormalizationFieldFailsToParse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "normalization"
                                              << "garbage"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(CollatorFactoryICUTest, NonBoolBackwardsFieldFailsToParse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "backwards"
                                              << "garbage"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(CollatorFactoryICUTest, VersionFieldParsesSuccessfully) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "version"
                                              << "57.1"));
    ASSERT_OK(collator.getStatus());
    ASSERT_EQ("57.1", collator.getValue()->getSpec().version);
}

TEST(CollatorFactoryICUTest, VersionFieldPopulatedWhenOmitted) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"));
    ASSERT_OK(collator.getStatus());
    ASSERT_EQ("57.1", collator.getValue()->getSpec().version);
}

TEST(CollatorFactoryICUTest, NonStringVersionFieldFailsToParse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "version"
                                              << 3));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(CollatorFactoryICUTest, UnknownCollatorVersionResultsInIncompatibleCollationVersionError) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "version"
                                              << "unknownVersion"));
    ASSERT_NOT_OK(collator.getStatus());
    ASSERT_EQ(collator.getStatus(), ErrorCodes::IncompatibleCollationVersion);
}

TEST(CollatorFactoryICUTest, FactoryMadeCollatorComparesStringsCorrectlyEnUS) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"));
    ASSERT_OK(collator.getStatus());

    ASSERT_LT(collator.getValue()->compare("ab", "ba"), 0);
    ASSERT_GT(collator.getValue()->compare("ba", "ab"), 0);
    ASSERT_EQ(collator.getValue()->compare("ab", "ab"), 0);
    ASSERT_LT(collator.getValue()->compare("a b", "ab"), 0);
    ASSERT_LT(collator.getValue()->compare("a-b", "ab"), 0);
}

TEST(CollatorFactoryICUTest, PrimaryStrengthCollatorIgnoresCaseAndAccents) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 1));
    ASSERT_OK(collator.getStatus());

    // u8"\u00E1" is latin small letter a with acute.
    ASSERT_EQ(collator.getValue()->compare("a", u8"\u00E1"), 0);
    ASSERT_EQ(collator.getValue()->compare("a", "A"), 0);
}

TEST(CollatorFactoryICUTest, SecondaryStrengthCollatorsIgnoresCaseButNotAccents) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 2));
    ASSERT_OK(collator.getStatus());

    // u8"\u00E1" is latin small letter a with acute.
    ASSERT_LT(collator.getValue()->compare("a", u8"\u00E1"), 0);
    ASSERT_EQ(collator.getValue()->compare("a", "A"), 0);
}

TEST(CollatorFactoryICUTest, TertiaryStrengthCollatorConsidersCaseAndAccents) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 3));
    ASSERT_OK(collator.getStatus());

    // u8"\u00E1" is latin small letter a with acute.
    ASSERT_LT(collator.getValue()->compare("a", u8"\u00E1"), 0);
    ASSERT_LT(collator.getValue()->compare("a", "A"), 0);
}

TEST(CollatorFactoryICUTest, PrimaryStrengthCaseLevelTrue) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 1
                                              << "caseLevel"
                                              << true));
    ASSERT_OK(collator.getStatus());

    // u8"\u00E1" is latin small letter a with acute.
    ASSERT_EQ(collator.getValue()->compare("a", u8"\u00E1"), 0);
    ASSERT_LT(collator.getValue()->compare("a", "A"), 0);
}

TEST(CollatorFactoryICUTest, PrimaryStrengthCaseLevelTrueCaseFirstUpper) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 1
                                              << "caseLevel"
                                              << true
                                              << "caseFirst"
                                              << "upper"));
    ASSERT_OK(collator.getStatus());

    // u8"\u00E1" is latin small letter a with acute.
    ASSERT_EQ(collator.getValue()->compare("a", u8"\u00E1"), 0);
    ASSERT_LT(collator.getValue()->compare("A", "a"), 0);
}

TEST(CollatorFactoryICUTest, TertiaryStrengthCaseLevelTrueCaseFirstUpper) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 3
                                              << "caseLevel"
                                              << true
                                              << "caseFirst"
                                              << "upper"));
    ASSERT_OK(collator.getStatus());
    ASSERT_LT(collator.getValue()->compare("A", "a"), 0);
}

TEST(CollatorFactoryICUTest, NumericOrderingFalse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"));
    ASSERT_OK(collator.getStatus());
    ASSERT_GT(collator.getValue()->compare("2", "10"), 0);
}

TEST(CollatorFactoryICUTest, NumericOrderingTrue) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "numericOrdering"
                                              << true));
    ASSERT_OK(collator.getStatus());
    ASSERT_LT(collator.getValue()->compare("2", "10"), 0);
}

TEST(CollatorFactoryICUTest, PrimaryStrengthAlternateShifted) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 1
                                              << "alternate"
                                              << "shifted"));
    ASSERT_OK(collator.getStatus());
    ASSERT_EQ(collator.getValue()->compare("a b", "ab"), 0);
    ASSERT_EQ(collator.getValue()->compare("a-b", "ab"), 0);
}

TEST(CollatorFactoryICUTest, QuaternaryStrengthAlternateShifted) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 4
                                              << "alternate"
                                              << "shifted"));
    ASSERT_OK(collator.getStatus());
    ASSERT_LT(collator.getValue()->compare("a b", "ab"), 0);
    ASSERT_LT(collator.getValue()->compare("a-b", "ab"), 0);
}

TEST(CollatorFactoryICUTest, PrimaryStrengthAlternateShiftedMaxVariableSpace) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 1
                                              << "alternate"
                                              << "shifted"
                                              << "maxVariable"
                                              << "space"));
    ASSERT_OK(collator.getStatus());
    ASSERT_EQ(collator.getValue()->compare("a b", "ab"), 0);
    ASSERT_LT(collator.getValue()->compare("a-b", "ab"), 0);
}

TEST(CollatorFactoryICUTest, SecondaryStrengthBackwardsFalse) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 2));
    ASSERT_OK(collator.getStatus());

    // u8"\u00E1" is latin small letter a with acute.
    ASSERT_LT(collator.getValue()->compare(u8"a\u00E1", u8"\u00E1a"), 0);
}

TEST(CollatorFactoryICUTest, SecondaryStrengthBackwardsTrue) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "strength"
                                              << 2
                                              << "backwards"
                                              << true));
    ASSERT_OK(collator.getStatus());

    // u8"\u00E1" is latin small letter a with acute.
    ASSERT_GT(collator.getValue()->compare(u8"a\u00E1", u8"\u00E1a"), 0);
}

TEST(CollatorFactoryICUTest, FactoryMadeCollatorComparisonKeysCorrectEnUS) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"));
    ASSERT_OK(collator.getStatus());
    const auto comparisonKeyAB = collator.getValue()->getComparisonKey("ab");
    const auto comparisonKeyABB = collator.getValue()->getComparisonKey("abb");
    const auto comparisonKeyBA = collator.getValue()->getComparisonKey("ba");

    ASSERT_LT(comparisonKeyAB.getKeyData().compare(comparisonKeyBA.getKeyData()), 0);
    ASSERT_GT(comparisonKeyBA.getKeyData().compare(comparisonKeyAB.getKeyData()), 0);
    ASSERT_EQ(comparisonKeyAB.getKeyData().compare(comparisonKeyAB.getKeyData()), 0);

    ASSERT_LT(comparisonKeyAB.getKeyData().compare(comparisonKeyABB.getKeyData()), 0);
    ASSERT_GT(comparisonKeyABB.getKeyData().compare(comparisonKeyAB.getKeyData()), 0);

    ASSERT_GT(comparisonKeyBA.getKeyData().compare(comparisonKeyABB.getKeyData()), 0);
    ASSERT_LT(comparisonKeyABB.getKeyData().compare(comparisonKeyBA.getKeyData()), 0);
}

TEST(CollatorFactoryICUTest, BackwardsTrueWithStrengthOneFails) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "backwards"
                                              << true
                                              << "strength"
                                              << 1));
    ASSERT_NOT_OK(collator.getStatus());
}

TEST(CollatorFactoryICUTest, BackwardsTrueWithStrengthTwoSucceeds) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "backwards"
                                              << true
                                              << "strength"
                                              << 2));
    ASSERT_OK(collator.getStatus());
}

TEST(CollatorFactoryICUTest, CaseFirstLowerWithStrengthThreeSucceeds) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "caseFirst"
                                              << "lower"
                                              << "strength"
                                              << 3));
    ASSERT_OK(collator.getStatus());
}

TEST(CollatorFactoryICUTest, CaseFirstUpperWithStrengthThreeSucceeds) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "caseFirst"
                                              << "upper"
                                              << "strength"
                                              << 3));
    ASSERT_OK(collator.getStatus());
}

TEST(CollatorFactoryICUTest, CaseFirstLowerWithCaseLevelSucceeds) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "caseFirst"
                                              << "lower"
                                              << "caseLevel"
                                              << true
                                              << "strength"
                                              << 1));
    ASSERT_OK(collator.getStatus());
}

TEST(CollatorFactoryICUTest, CaseFirstUpperWithCaseLevelSucceeds) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "caseFirst"
                                              << "upper"
                                              << "caseLevel"
                                              << true
                                              << "strength"
                                              << 1));
    ASSERT_OK(collator.getStatus());
}

TEST(CollatorFactoryICUTest, CaseFirstOffWithStrengthOneSucceeds) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "caseFirst"
                                              << "off"
                                              << "strength"
                                              << 1));
    ASSERT_OK(collator.getStatus());
}

TEST(CollatorFactoryICUTest, CaseFirstLowerWithStrengthOneFails) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "caseFirst"
                                              << "lower"
                                              << "strength"
                                              << 1));
    ASSERT_NOT_OK(collator.getStatus());
}

TEST(CollatorFactoryICUTest, CaseFirstLowerWithStrengthTwoFails) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "caseFirst"
                                              << "lower"
                                              << "strength"
                                              << 2));
    ASSERT_NOT_OK(collator.getStatus());
}

TEST(CollatorFactoryICUTest, CaseFirstUpperWithStrengthOneFails) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "caseFirst"
                                              << "upper"
                                              << "strength"
                                              << 1));
    ASSERT_NOT_OK(collator.getStatus());
}

TEST(CollatorFactoryICUTest, CaseFirstUpperWithStrengthTwoFails) {
    CollatorFactoryICU factory;
    auto collator = factory.makeFromBSON(BSON("locale"
                                              << "en_US"
                                              << "caseFirst"
                                              << "upper"
                                              << "strength"
                                              << 2));
    ASSERT_NOT_OK(collator.getStatus());
}

TEST(CollatorFactoryICUTest, GermanPhonebookCollatoHasExpectedComparisonSemantics) {
    CollatorFactoryICU factory;
    auto dictionaryCollator = factory.makeFromBSON(BSON("locale"
                                                        << "de"));
    ASSERT_OK(dictionaryCollator.getStatus());
    auto phonebookCollator = factory.makeFromBSON(BSON("locale"
                                                       << "de@collation=phonebook"));
    ASSERT_OK(phonebookCollator.getStatus());

    StringData withoutAccent("of");
    StringData withAccent("öf");

    ASSERT_LT(dictionaryCollator.getValue()->compare(withoutAccent, withAccent), 0);
    ASSERT_GT(phonebookCollator.getValue()->compare(withoutAccent, withAccent), 0);
}

TEST(CollatorFactoryICUTest, AliasNoForNorwegianBokmalLocaleNotSupported) {
    CollatorFactoryICU factory;
    ASSERT_NOT_OK(factory
                      .makeFromBSON(BSON("locale"
                                         << "no"))
                      .getStatus());
}

TEST(CollatorFactoryICUTest, TraditionalSpanishAliasNotSupported) {
    CollatorFactoryICU factory;
    ASSERT_NOT_OK(factory
                      .makeFromBSON(BSON("locale"
                                         << "es__TRADITIONAL"))
                      .getStatus());
}

}  // namespace
