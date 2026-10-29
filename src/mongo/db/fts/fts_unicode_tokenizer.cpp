// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/fts/fts_unicode_tokenizer.h"

#include "mongo/db/fts/stemmer.h"
#include "mongo/db/fts/stop_words.h"

#include <string>
#include <string_view>

namespace mongo {
namespace fts {

using std::string;

UnicodeFTSTokenizer::UnicodeFTSTokenizer(const FTSLanguage* language)
    : _language(language),
      _stemmer(language),
      _stopWords(StopWords::getStopWords(language)),
      _delimListLanguage(_language->str() == "english"
                             ? unicode::DelimiterListLanguage::kEnglish
                             : unicode::DelimiterListLanguage::kNotEnglish),
      _caseFoldMode(_language->str() == "turkish" ? unicode::CaseFoldMode::kTurkish
                                                  : unicode::CaseFoldMode::kNormal) {}

void UnicodeFTSTokenizer::reset(std::string_view document, Options options) {
    _options = options;
    _pos = 0;
    _document.resetData(document);  // Validates that document is valid UTF8.

    // Skip any leading delimiters (and handle the case where the document is entirely delimiters).
    _skipDelimiters();
}

bool UnicodeFTSTokenizer::moveNext() {
    while (true) {
        if (_pos >= _document.size()) {
            _word = "";
            return false;
        }

        // Traverse through non-delimiters and build the next token.
        size_t start = _pos++;
        while (_pos < _document.size() &&
               (!unicode::codepointIsDelimiter(_document[_pos], _delimListLanguage))) {
            ++_pos;
        }
        const size_t len = _pos - start;

        // Skip the delimiters before the next token.
        _skipDelimiters();

        // Stop words are case-sensitive and diacritic sensitive, so we need them to be lower cased
        // but with diacritics not removed to check against the stop word list.
        _word = _document.toLowerToBuf(&_wordBuf, _caseFoldMode, start, len);

        if ((_options & kFilterStopWords) && _stopWords->isStopWord(_word)) {
            continue;
        }

        if (_options & kGenerateCaseSensitiveTokens) {
            _word = _document.substrToBuf(&_wordBuf, start, len);
        }

        // The stemmer is diacritic sensitive, so stem the word before removing diacritics.
        _word = _stemmer.stem(_word);

        if (!(_options & kGenerateDiacriticSensitiveTokens)) {
            // Can't use _wordbuf for output here because our input _word may point into it.
            _word = unicode::String::caseFoldAndStripDiacritics(
                &_finalBuf, _word, unicode::String::kCaseSensitive, _caseFoldMode);
        }

        return true;
    }
}

std::string_view UnicodeFTSTokenizer::get() const {
    return _word;
}

void UnicodeFTSTokenizer::_skipDelimiters() {
    while (_pos < _document.size() &&
           unicode::codepointIsDelimiter(_document[_pos], _delimListLanguage)) {
        ++_pos;
    }
}

}  // namespace fts
}  // namespace mongo
