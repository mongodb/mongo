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

#include "mongo/db/fts/fts_basic_tokenizer.h"

#include "mongo/db/fts/fts_query_impl.h"
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

BasicFTSTokenizer::BasicFTSTokenizer(const FTSLanguage* language)
    : _language(language), _stemmer(language), _stopWords(StopWords::getStopWords(language)) {}

void BasicFTSTokenizer::reset(StringData document, Options options) {
    _options = options;
    _document = document.toString();
    _tokenizer = stdx::make_unique<Tokenizer>(_language, _document);
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

        string word = tolowerString(token.data);

        // Stop words are case-sensitive so we need them to be lower cased to check
        // against the stop word list
        if ((_options & FTSTokenizer::kFilterStopWords) && _stopWords->isStopWord(word)) {
            continue;
        }

        if (_options & FTSTokenizer::kGenerateCaseSensitiveTokens) {
            word = token.data.toString();
        }

        _stem = _stemmer.stem(word).toString();
        return true;
    }
}

StringData BasicFTSTokenizer::get() const {
    return _stem;
}

}  // namespace fts
}  // namespace mongo
