// fts_language_test.cpp

/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/db/fts/fts_language.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/platform/basic.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace fts {

// Positive tests for FTSLanguage::make() with TEXT_INDEX_VERSION_3.

TEST(FTSLanguageV3, ExactLanguage) {
    StatusWithFTSLanguage swl = FTSLanguage::make("spanish", TEXT_INDEX_VERSION_3);
    ASSERT(swl.getStatus().isOK());
    ASSERT_EQUALS(swl.getValue()->str(), "spanish");
}

TEST(FTSLanguageV3, ExactCode) {
    StatusWithFTSLanguage swl = FTSLanguage::make("es", TEXT_INDEX_VERSION_3);
    ASSERT(swl.getStatus().isOK());
    ASSERT_EQUALS(swl.getValue()->str(), "spanish");
}

TEST(FTSLanguageV3, UpperCaseLanguage) {
    StatusWithFTSLanguage swl = FTSLanguage::make("SPANISH", TEXT_INDEX_VERSION_3);
    ASSERT(swl.getStatus().isOK());
    ASSERT_EQUALS(swl.getValue()->str(), "spanish");
}

TEST(FTSLanguageV3, UpperCaseCode) {
    StatusWithFTSLanguage swl = FTSLanguage::make("ES", TEXT_INDEX_VERSION_3);
    ASSERT(swl.getStatus().isOK());
    ASSERT_EQUALS(swl.getValue()->str(), "spanish");
}

TEST(FTSLanguageV3, NoneLanguage) {
    StatusWithFTSLanguage swl = FTSLanguage::make("none", TEXT_INDEX_VERSION_3);
    ASSERT(swl.getStatus().isOK());
    ASSERT_EQUALS(swl.getValue()->str(), "none");
}

// Negative tests for FTSLanguage::make() with TEXT_INDEX_VERSION_3.

TEST(FTSLanguageV3, Empty) {
    StatusWithFTSLanguage swl = FTSLanguage::make("", TEXT_INDEX_VERSION_3);
    ASSERT(!swl.getStatus().isOK());
}

TEST(FTSLanguageV3, Unknown) {
    StatusWithFTSLanguage swl = FTSLanguage::make("spanglish", TEXT_INDEX_VERSION_3);
    ASSERT(!swl.getStatus().isOK());
}

// Positive tests for FTSLanguage::make() with TEXT_INDEX_VERSION_2.

TEST(FTSLanguageV2, ExactLanguage) {
    StatusWithFTSLanguage swl = FTSLanguage::make("spanish", TEXT_INDEX_VERSION_2);
    ASSERT(swl.getStatus().isOK());
    ASSERT_EQUALS(swl.getValue()->str(), "spanish");
}

TEST(FTSLanguageV2, ExactCode) {
    StatusWithFTSLanguage swl = FTSLanguage::make("es", TEXT_INDEX_VERSION_2);
    ASSERT(swl.getStatus().isOK());
    ASSERT_EQUALS(swl.getValue()->str(), "spanish");
}

TEST(FTSLanguageV2, UpperCaseLanguage) {
    StatusWithFTSLanguage swl = FTSLanguage::make("SPANISH", TEXT_INDEX_VERSION_2);
    ASSERT(swl.getStatus().isOK());
    ASSERT_EQUALS(swl.getValue()->str(), "spanish");
}

TEST(FTSLanguageV2, UpperCaseCode) {
    StatusWithFTSLanguage swl = FTSLanguage::make("ES", TEXT_INDEX_VERSION_2);
    ASSERT(swl.getStatus().isOK());
    ASSERT_EQUALS(swl.getValue()->str(), "spanish");
}

TEST(FTSLanguageV2, NoneLanguage) {
    StatusWithFTSLanguage swl = FTSLanguage::make("none", TEXT_INDEX_VERSION_2);
    ASSERT(swl.getStatus().isOK());
    ASSERT_EQUALS(swl.getValue()->str(), "none");
}

// Negative tests for FTSLanguage::make() with TEXT_INDEX_VERSION_2.

TEST(FTSLanguageV2, Unknown) {
    StatusWithFTSLanguage swl = FTSLanguage::make("spanglish", TEXT_INDEX_VERSION_2);
    ASSERT(!swl.getStatus().isOK());
}

TEST(FTSLanguageV2, Empty) {
    StatusWithFTSLanguage swl = FTSLanguage::make("", TEXT_INDEX_VERSION_2);
    ASSERT(!swl.getStatus().isOK());
}

// Positive tests for FTSLanguage::make() with TEXT_INDEX_VERSION_1.

TEST(FTSLanguageV1, ExactLanguage) {
    StatusWithFTSLanguage swl = FTSLanguage::make("spanish", TEXT_INDEX_VERSION_1);
    ASSERT(swl.getStatus().isOK());
    ASSERT_EQUALS(swl.getValue()->str(), "spanish");
}

TEST(FTSLanguageV1, DeprecatedLanguage) {
    StatusWithFTSLanguage swl = FTSLanguage::make("porter", TEXT_INDEX_VERSION_1);
    ASSERT(swl.getStatus().isOK());
    ASSERT_EQUALS(swl.getValue()->str(), "porter");
}

TEST(FTSLanguageV1, StemmerOnlyLanguage1) {
    StatusWithFTSLanguage swl = FTSLanguage::make("en", TEXT_INDEX_VERSION_1);
    ASSERT(swl.getStatus().isOK());
    ASSERT_EQUALS(swl.getValue()->str(), "en");
}

TEST(FTSLanguageV1, StemmerOnlyLanguage2) {
    StatusWithFTSLanguage swl = FTSLanguage::make("eng", TEXT_INDEX_VERSION_1);
    ASSERT(swl.getStatus().isOK());
    ASSERT_EQUALS(swl.getValue()->str(), "eng");
}

TEST(FTSLanguageV1, NoneLanguage) {
    StatusWithFTSLanguage swl = FTSLanguage::make("none", TEXT_INDEX_VERSION_1);
    ASSERT(swl.getStatus().isOK());
    ASSERT_EQUALS(swl.getValue()->str(), "none");
}

// Negative tests for FTSLanguage::make() with TEXT_INDEX_VERSION_1.

TEST(FTSLanguageV1, CaseSensitive) {
    StatusWithFTSLanguage swl = FTSLanguage::make("SPANISH", TEXT_INDEX_VERSION_1);
    ASSERT(swl.getStatus().isOK());
    ASSERT_EQUALS(swl.getValue()->str(), "none");
}

TEST(FTSLanguageV1, Unknown) {
    StatusWithFTSLanguage swl = FTSLanguage::make("asdf", TEXT_INDEX_VERSION_1);
    ASSERT(swl.getStatus().isOK());
    ASSERT_EQUALS(swl.getValue()->str(), "none");
}

TEST(FTSLanguageV1, Empty) {
    StatusWithFTSLanguage swl = FTSLanguage::make("", TEXT_INDEX_VERSION_1);
    ASSERT(swl.getStatus().isOK());
    ASSERT_EQUALS(swl.getValue()->str(), "none");
}
}
}
