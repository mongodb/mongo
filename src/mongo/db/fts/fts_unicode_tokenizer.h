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

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/db/fts/fts_tokenizer.h"
#include "mongo/db/fts/stemmer.h"
#include "mongo/db/fts/tokenizer.h"
#include "mongo/db/fts/unicode/string.h"

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
    MONGO_DISALLOW_COPYING(UnicodeFTSTokenizer);

public:
    UnicodeFTSTokenizer(const FTSLanguage* language);

    void reset(StringData document, Options options) override;

    bool moveNext() override;

    StringData get() const override;

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
    StringData _word;
    Options _options;

    StackBufBuilder _wordBuf;
    StackBufBuilder _finalBuf;
};

}  // namespace fts
}  // namespace mongo
