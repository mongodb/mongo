// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/fts/fts_unicode_phrase_matcher.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace fts {

// Case insensitive & diacritic insensitive match.
TEST(FtsUnicodePhraseMatcher, CaseAndDiacriticInsensitive) {
    std::string str =
        "El pingüino Wenceslao hizo kilómetros bajo exhaustiva lluvia y frío, añoraba";
    std::string find1 = "pinguino wenceslao";
    std::string find2 = "frio, anoraba";

    std::string nofind1 = "bajo lluvia";
    std::string nofind2 = "El Wenceslao";

    UnicodeFTSPhraseMatcher phraseMatcher("spanish");
    FTSPhraseMatcher::Options options = FTSPhraseMatcher::kNone;

    ASSERT(phraseMatcher.phraseMatches(find1, str, options));
    ASSERT(phraseMatcher.phraseMatches(find2, str, options));

    ASSERT_FALSE(phraseMatcher.phraseMatches(nofind1, str, options));
    ASSERT_FALSE(phraseMatcher.phraseMatches(nofind2, str, options));
}

// Case sensitive & diacritic insensitive match.
TEST(FtsUnicodePhraseMatcher, CaseSensitiveAndDiacriticInsensitive) {
    std::string str =
        "El pingüino Wenceslao hizo kilómetros bajo exhaustiva lluvia y frío, añoraba";
    std::string find1 = "pinguino Wenceslao";
    std::string find2 = "El pinguino";

    std::string nofind1 = "pinguino wenceslao";
    std::string nofind2 = "el pinguino";

    UnicodeFTSPhraseMatcher phraseMatcher("spanish");
    FTSPhraseMatcher::Options options = FTSPhraseMatcher::kCaseSensitive;

    ASSERT(phraseMatcher.phraseMatches(find1, str, options));
    ASSERT(phraseMatcher.phraseMatches(find2, str, options));

    ASSERT_FALSE(phraseMatcher.phraseMatches(nofind1, str, options));
    ASSERT_FALSE(phraseMatcher.phraseMatches(nofind2, str, options));
}

// Case insensitive & diacritic sensitive match.
TEST(FtsUnicodePhraseMatcher, CaseInsensitiveAndDiacriticSensitive) {
    std::string str =
        "El pingüino Wenceslao hizo kilómetros bajo exhaustiva lluvia y frío, añoraba";
    std::string find1 = "HIZO KILÓMETROS";
    std::string find2 = "el pingüino";

    std::string nofind1 = "hizo kilometros";
    std::string nofind2 = "pinguino";

    UnicodeFTSPhraseMatcher phraseMatcher("spanish");
    FTSPhraseMatcher::Options options = FTSPhraseMatcher::kDiacriticSensitive;

    ASSERT(phraseMatcher.phraseMatches(find1, str, options));
    ASSERT(phraseMatcher.phraseMatches(find2, str, options));

    ASSERT_FALSE(phraseMatcher.phraseMatches(nofind1, str, options));
    ASSERT_FALSE(phraseMatcher.phraseMatches(nofind2, str, options));
}

// Case sensitive & diacritic sensitive match.
TEST(FtsUnicodePhraseMatcher, CaseAndDiacriticSensitive) {
    std::string str =
        "El pingüino Wenceslao hizo kilómetros bajo exhaustiva lluvia y frío, añoraba";
    std::string find1 = "pingüino Wenceslao";
    std::string find2 = "kilómetros bajo";

    std::string nofind1 = "pinguino Wenceslao";
    std::string nofind2 = "kilómetros BaJo";

    UnicodeFTSPhraseMatcher phraseMatcher("spanish");
    FTSPhraseMatcher::Options options =
        FTSPhraseMatcher::kCaseSensitive | FTSPhraseMatcher::kDiacriticSensitive;

    ASSERT(phraseMatcher.phraseMatches(find1, str, options));
    ASSERT(phraseMatcher.phraseMatches(find2, str, options));

    ASSERT_FALSE(phraseMatcher.phraseMatches(nofind1, str, options));
    ASSERT_FALSE(phraseMatcher.phraseMatches(nofind2, str, options));
}

// Case insensitive & diacritic insensitive match.
TEST(FtsUnicodePhraseMatcher, CaseAndDiacriticInsensitiveTurkish) {
    std::string str = "Pijamalı hasta yağız şoföre çabucak güvendi.";
    std::string find1 = "PİJAMALI hasta";
    std::string find2 = "YAGIZ sofore";

    std::string nofind1 = "çabucak GÜVENDI";
    std::string nofind2 = "yagiz sofore";

    UnicodeFTSPhraseMatcher phraseMatcher("turkish");
    FTSPhraseMatcher::Options options = FTSPhraseMatcher::kNone;

    ASSERT(phraseMatcher.phraseMatches(find1, str, options));
    ASSERT(phraseMatcher.phraseMatches(find2, str, options));

    ASSERT_FALSE(phraseMatcher.phraseMatches(nofind1, str, options));
    ASSERT_FALSE(phraseMatcher.phraseMatches(nofind2, str, options));
}


}  // namespace fts
}  // namespace mongo
