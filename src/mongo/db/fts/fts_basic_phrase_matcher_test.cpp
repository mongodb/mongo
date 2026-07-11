// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/fts/fts_basic_phrase_matcher.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace fts {

// Case insensitive match.
TEST(FtsBasicPhraseMatcher, CaseInsensitive) {
    std::string str1 = "Lorem ipsum dolor sit amet, consectetur adipiscing elit.";
    std::string find1 = "Consectetur adipiscing";
    std::string nofind1 = "dolor amet";

    std::string str2 = "Duis aute irure dolor in reprehenderit in Voluptate velit esse cillum.";
    std::string find2 = "In Voluptate";
    std::string nofind2 = "dolor velit";

    BasicFTSPhraseMatcher phraseMatcher;
    FTSPhraseMatcher::Options options = FTSPhraseMatcher::kNone;

    ASSERT(phraseMatcher.phraseMatches(find1, str1, options));
    ASSERT(phraseMatcher.phraseMatches(find2, str2, options));

    ASSERT_FALSE(phraseMatcher.phraseMatches(nofind1, str1, options));
    ASSERT_FALSE(phraseMatcher.phraseMatches(nofind2, str2, options));
}

// Case sensitive match.
TEST(FtsBasicPhraseMatcher, CaseSensitive) {
    std::string str1 = "Lorem ipsum dolor sit amet, consectetur adipiscing elit.";
    std::string find1 = "Lorem ipsum";
    std::string nofind1 = "Sit amet";

    std::string str2 = "Duis aute irure dolor in reprehenderit in Voluptate velit esse cillum.";
    std::string find2 = "in Voluptate";
    std::string nofind2 = "Irure dolor";

    BasicFTSPhraseMatcher phraseMatcher;
    FTSPhraseMatcher::Options options = FTSPhraseMatcher::kCaseSensitive;

    ASSERT(phraseMatcher.phraseMatches(find1, str1, options));
    ASSERT(phraseMatcher.phraseMatches(find2, str2, options));

    ASSERT_FALSE(phraseMatcher.phraseMatches(nofind1, str1, options));
    ASSERT_FALSE(phraseMatcher.phraseMatches(nofind2, str2, options));
}

}  // namespace fts
}  // namespace mongo
