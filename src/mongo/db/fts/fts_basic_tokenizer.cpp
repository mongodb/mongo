// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/fts/fts_basic_tokenizer.h"

#include "mongo/db/fts/stemmer.h"
#include "mongo/db/fts/stop_words.h"
#include "mongo/db/fts/tokenizer.h"
#include "mongo/util/str.h"

#include <memory>
#include <string_view>

namespace mongo {
namespace fts {

using std::string;

BasicFTSTokenizer::BasicFTSTokenizer(const FTSLanguage* language)
    : _language(language), _stemmer(language), _stopWords(StopWords::getStopWords(language)) {}

void BasicFTSTokenizer::reset(std::string_view document, Options options) {
    _options = options;
    _document = std::string{document};
    _tokenizer = std::make_unique<Tokenizer>(_language, _document);
}

bool BasicFTSTokenizer::moveNext() {
    while (true) {
        bool hasMore = _tokenizer->more();
        if (!hasMore) {
            _stem = "";
            return false;
        }

        Token token = _tokenizer->next();

        // Do not return delimiters
        if (token.type != Token::TEXT) {
            continue;
        }

        string word = str::toLower(token.data);

        // Stop words are case-sensitive so we need them to be lower cased to check
        // against the stop word list
        if ((_options & FTSTokenizer::kFilterStopWords) && _stopWords->isStopWord(word)) {
            continue;
        }

        if (_options & FTSTokenizer::kGenerateCaseSensitiveTokens) {
            word = std::string{token.data};
        }

        _stem = std::string{_stemmer.stem(word)};
        return true;
    }
}

std::string_view BasicFTSTokenizer::get() const {
    return _stem;
}

}  // namespace fts
}  // namespace mongo
