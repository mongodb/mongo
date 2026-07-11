// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/fts/fts_language.h"
#include "mongo/db/fts/fts_tokenizer.h"
#include "mongo/db/fts/stemmer.h"
#include "mongo/db/fts/tokenizer.h"
#include "mongo/db/fts/unicode/codepoints.h"
#include "mongo/db/fts/unicode/string.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string_view>

namespace mongo {
namespace fts {

class FTSLanguage;
class StopWords;

/**
 * UnicodeFTSTokenizer
 * A iterator of "documents" where a document contains words delimited by a predefined set of
 * Unicode delimiters (see gen_delimiter_list.py)
 * Uses
 * - A list of Unicode delimiters for tokenizing words (see gen_delimiter_list.py).
 * - tolower from mongo::unicode, which supports UTF-8 simple and Turkish case folding
 * - Stemmer (ie, Snowball Stemmer) to stem words.
 * - Embeded stop word lists for each language in StopWord class
 *
 * For each word returns a stem version of a word optimized for full text indexing.
 * Optionally supports returning case sensitive search terms.
 */
class UnicodeFTSTokenizer final : public FTSTokenizer {
    UnicodeFTSTokenizer(const UnicodeFTSTokenizer&) = delete;
    UnicodeFTSTokenizer& operator=(const UnicodeFTSTokenizer&) = delete;

public:
    UnicodeFTSTokenizer(const FTSLanguage* language);

    void reset(std::string_view document, Options options) override;

    bool moveNext() override;

    std::string_view get() const override;

private:
    /**
     * Helper that moves the tokenizer past all delimiters that shouldn't be considered part of
     * tokens.
     */
    void _skipDelimiters();

    const FTSLanguage* const _language;
    const Stemmer _stemmer;
    const StopWords* const _stopWords;
    const unicode::DelimiterListLanguage _delimListLanguage;
    const unicode::CaseFoldMode _caseFoldMode;

    unicode::String _document;
    size_t _pos;
    std::string_view _word;
    Options _options;

    StackBufBuilder _wordBuf;
    StackBufBuilder _finalBuf;
};

}  // namespace fts
}  // namespace mongo
