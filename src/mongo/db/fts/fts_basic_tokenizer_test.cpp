// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/fts/fts_language.h"
#include "mongo/db/fts/fts_tokenizer.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mongo {
namespace fts {

using namespace std::string_view_literals;
using testing::ElementsAre;

std::vector<std::string> tokenizeString(std::string_view str, std::string_view language) {
    // To retrieve the FTSBasicTokenizer, use TEXT_INDEX_VERSION_2
    auto tokenizer = FTSLanguage::make(language, TEXT_INDEX_VERSION_2).createTokenizer();

    tokenizer->reset(str, FTSTokenizer::kNone);

    std::vector<std::string> terms;

    while (tokenizer->moveNext()) {
        terms.push_back(std::string{tokenizer->get()});
    }

    return terms;
}

// Ensure punctuation is filtered out of the indexed document
// and the 's is not separated
TEST(FtsBasicTokenizer, English) {
    ASSERT_THAT(tokenizeString("Do you see Mark's dog running?", "english"),
                ElementsAre("do", "you", "see", "mark", "dog", "run"));
}

// Ensure punctuation is filtered out of the indexed document
// and the 's is separated
TEST(FtsBasicTokenizer, French) {
    ASSERT_THAT(tokenizeString("Do you see Mark's dog running?", "french"),
                ElementsAre("do", "you", "se", "mark", "s", "dog", "running"));
}

TEST(FtsBasicTokenizer, EmbeddedNul) {
    ASSERT_THAT(tokenizeString("abc\0def"sv, "none"), ElementsAre("abc\0def"sv));
}

}  // namespace fts
}  // namespace mongo
