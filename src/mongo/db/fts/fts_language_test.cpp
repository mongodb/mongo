// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/fts/fts_language.h"

#include "mongo/base/error_codes.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <ostream>

namespace mongo {

namespace fts {

namespace {

using LanguageMakeException = mongo::ExceptionFor<ErrorCodes::BadValue>;

TEST(FTSLanguageV3, Make) {
    static constexpr auto kVer = TEXT_INDEX_VERSION_3;
    ASSERT_EQUALS(FTSLanguage::make("spanish", kVer).str(), "spanish");
    ASSERT_EQUALS(FTSLanguage::make("es", kVer).str(), "spanish");
    ASSERT_EQUALS(FTSLanguage::make("SPANISH", kVer).str(), "spanish");
    ASSERT_EQUALS(FTSLanguage::make("ES", kVer).str(), "spanish");
    ASSERT_EQUALS(FTSLanguage::make("none", kVer).str(), "none");
    ASSERT_THROWS(FTSLanguage::make("", kVer), LanguageMakeException);
    ASSERT_THROWS(FTSLanguage::make("spanglish", kVer), LanguageMakeException);
}

TEST(FTSLanguageV2, Make) {
    static constexpr auto kVer = TEXT_INDEX_VERSION_2;
    ASSERT_EQUALS(FTSLanguage::make("spanish", kVer).str(), "spanish");
    ASSERT_EQUALS(FTSLanguage::make("es", kVer).str(), "spanish");
    ASSERT_EQUALS(FTSLanguage::make("SPANISH", kVer).str(), "spanish");
    ASSERT_EQUALS(FTSLanguage::make("ES", kVer).str(), "spanish");
    ASSERT_EQUALS(FTSLanguage::make("none", kVer).str(), "none");
    ASSERT_THROWS(FTSLanguage::make("spanglish", kVer), LanguageMakeException);
    ASSERT_THROWS(FTSLanguage::make("", kVer), LanguageMakeException);
}

TEST(FTSLanguageV1, Make) {
    static constexpr auto kVer = TEXT_INDEX_VERSION_1;
    ASSERT_EQUALS(FTSLanguage::make("spanish", kVer).str(), "spanish");
    ASSERT_EQUALS(FTSLanguage::make("porter", kVer).str(), "porter") << "deprecated";
    ASSERT_EQUALS(FTSLanguage::make("en", kVer).str(), "en");
    ASSERT_EQUALS(FTSLanguage::make("eng", kVer).str(), "eng");
    ASSERT_EQUALS(FTSLanguage::make("none", kVer).str(), "none");
    // Negative V1 tests
    ASSERT_EQUALS(FTSLanguage::make("SPANISH", kVer).str(), "none") << "case sensitive";
    ASSERT_EQUALS(FTSLanguage::make("asdf", kVer).str(), "none") << "unknown";
    ASSERT_EQUALS(FTSLanguage::make("", kVer).str(), "none");
}

}  // namespace
}  // namespace fts
}  // namespace mongo
