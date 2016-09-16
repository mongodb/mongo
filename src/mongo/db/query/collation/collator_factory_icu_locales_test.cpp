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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithAfrikaansLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "af"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithAmharicLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "am"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithArabicLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "ar"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithAssameseLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "as"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithAzerbaijaniLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "az"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithBelarusianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "be"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithBulgarianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "bg"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithBengaliLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "bn"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithTibetanLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "bo"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithBosnianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "bs"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithBosnianCyrillicLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "bs_Cyrl"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithCatalanLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "ca"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithCherokeeLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "chr"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithCzechLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "cs"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithWelshLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "cy"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithDanishLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "da"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithGermanLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "de"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithGermanAustriaLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "de_AT"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithLowerSorbianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "dsb"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithDzongkhaLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "dz"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithEweLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "ee"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithGreekLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "el"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithEnglishLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "en"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithEnglishUnitedStatesLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "en_US"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest,
     FactoryInitializationSucceedsWithEnglishUnitedStatesComputerLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "en_US_POSIX"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithEsperantoLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "eo"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithSpanishLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "es"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithEstonianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "et"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithPersianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "fa"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithPersianAfghanistanLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "fa_AF"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithFinnishLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "fi"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithFilipinoLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "fil"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithFaroeseLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "fo"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithFrenchLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "fr"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithFrenchCanadaLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "fr_CA"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithIrishLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "ga"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithGalicianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "gl"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithGujaratiLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "gu"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithHausaLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "ha"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithHawaiianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "haw"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithHebrewLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "he"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithHindiLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "hi"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithCroatianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "hr"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithUpperSorbianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "hsb"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithHungarianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "hu"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithArmenianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "hy"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithIndonesianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "id"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithIgboLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "ig"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithIcelandicLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "is"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithItalianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "it"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithJapaneseLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "ja"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithGeorgianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "ka"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithKazakhLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "kk"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithKalaallisutLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "kl"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithKhmerLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "km"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithKannadaLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "kn"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithKoreanLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "ko"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithKonkaniLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "kok"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithKyrgyzLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "ky"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithLuxembourgishLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "lb"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithLakotaLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "lkt"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithLingalaLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "ln"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithLaoLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "lo"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithLithuanianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "lt"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithLatvianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "lv"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithMacedonianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "mk"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithMalayalamLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "ml"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithMongolianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "mn"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithMarathiLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "mr"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithMalayLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "ms"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithMalteseLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "mt"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithBurmeseLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "my"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithNorwegianBokmalLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "nb"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithNepaliLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "ne"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithDutchLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "nl"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithNorwegianNynorskLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "nn"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithOromoLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "om"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithOriyaLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "or"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithPunjabiLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "pa"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithPolishLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "pl"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithPashtoLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "ps"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithPortugueseLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "pt"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithRomanianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "ro"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithRussianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "ru"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithNorthernSamiLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "se"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithSinhalaLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "si"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithSlovakLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "sk"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithSlovenianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "sl"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithInariSamiLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "smn"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithAlbanianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "sq"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithSerbianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "sr"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithSerbianLatinLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "sr_Latn"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithSwedishLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "sv"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithSwahiliLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "sw"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithTamilLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "ta"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithTeluguLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "te"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithThaiLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "th"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithTonganLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "to"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithTurkishLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "tr"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithUyghurLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "ug"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithUkrainianLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "uk"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithUrduLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "ur"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithVietnameseLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "vi"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithWalserLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "wae"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithYiddishLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "yi"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithYorubaLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "yo"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithChineseLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "zh"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithChineseTraditionalLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "zh_Hant"))
                  .getStatus());
}

TEST(CollatorFactoryICULocalesTest, FactoryInitializationSucceedsWithZuluLocale) {
    CollatorFactoryICU factory;
    ASSERT_OK(factory
                  .makeFromBSON(BSON("locale"
                                     << "zu"))
                  .getStatus());
}

}  // namespace
}  // namespace mongo
