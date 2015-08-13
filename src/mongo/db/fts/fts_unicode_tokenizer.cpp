/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/db/fts/fts_unicode_tokenizer.h"

#include "mongo/db/fts/fts_query.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/fts/stemmer.h"
#include "mongo/db/fts/stop_words.h"
#include "mongo/db/fts/tokenizer.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stringutils.h"

namespace mongo {
namespace fts {

using std::string;

UnicodeFTSTokenizer::UnicodeFTSTokenizer(const FTSLanguage* language)
    : _language(language), _stemmer(language), _stopWords(StopWords::getStopWords(language)) {
    if (_language->str() == "english") {
        _delimListLanguage = unicode::DelimiterListLanguage::kEnglish;
    } else {
        _delimListLanguage = unicode::DelimiterListLanguage::kNotEnglish;
    }

    if (_language->str() == "turkish") {
        _caseFoldMode = unicode::CaseFoldMode::kTurkish;
    } else {
        _caseFoldMode = unicode::CaseFoldMode::kNormal;
    }
}

void UnicodeFTSTokenizer::reset(StringData document, Options options) {
    _options = options;
    _pos = 0;
    _document.resetData(document);

    // Skip any leading delimiters (and handle the case where the document is entirely delimiters).
    _skipDelimiters();
}

bool UnicodeFTSTokenizer::moveNext() {
    while (true) {
        if (_pos >= _document.size()) {
            _stem = "";
            return false;
        }

        // Traverse through non-delimiters and build the next token.
        size_t start = _pos++;
        while (_pos < _document.size() &&
               (!unicode::codepointIsDelimiter(_document[_pos], _delimListLanguage))) {
            ++_pos;
        }
        _document.substrToBuf(start, _pos - start, _tokenBuf);

        // Skip the delimiters before the next token.
        _skipDelimiters();

        // Stop words are case-sensitive and diacritic sensitive, so we need them to be lower cased
        // but with diacritics not removed to check against the stop word list.
        _tokenBuf.toLowerToBuf(_caseFoldMode, _wordBuf);

        if ((_options & kFilterStopWords) && _stopWords->isStopWord(_wordBuf.toString())) {
            continue;
        }

        if (_options & kGenerateCaseSensitiveTokens) {
            _tokenBuf.copyToBuf(_wordBuf);
        }

        // The stemmer is diacritic sensitive, so stem the word before removing diacritics.
        _stem = _stemmer.stem(_wordBuf.toString());

        if (!(_options & kGenerateDiacriticSensitiveTokens)) {
            _tokenBuf.resetData(_stem);
            _tokenBuf.removeDiacriticsToBuf(_wordBuf);
            _stem = _wordBuf.toString();
        }

        return true;
    }
}

StringData UnicodeFTSTokenizer::get() const {
    return _stem;
}

void UnicodeFTSTokenizer::_skipDelimiters() {
    while (_pos < _document.size() &&
           unicode::codepointIsDelimiter(_document[_pos], _delimListLanguage)) {
        ++_pos;
    }
}

}  // namespace fts
}  // namespace mongo
