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

#include <cstdint>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"

namespace mongo {
namespace fts {

class FTSLanguage;
class StopWords;

/**
 * FTSTokenizer
 * A iterator of "documents" where a document contains space delimited words. For each word returns
 * a stem or lemma version of a word optimized for full text indexing. Supports various options to
 * control how tokens are generated.
 */
class FTSTokenizer {
public:
    virtual ~FTSTokenizer() = default;

    /**
     * Options for generating tokens.
     */
    using Options = uint8_t;

    /**
     * Default means lower cased, diacritics removed, and stop words are not filtered.
     */
    static const Options kNone = 0;

    /**
     * Do not lower case terms.
     */
    static const Options kGenerateCaseSensitiveTokens = 1 << 0;

    /**
     * Filter out stop words from return tokens.
     */
    static const Options kFilterStopWords = 1 << 1;

    /**
     * Do not remove diacritics from terms.
     */
    static const Options kGenerateDiacriticSensitiveTokens = 1 << 2;

    /**
     * Process a new document, and discards any previous results.
     * May be called multiple times on an instance of an iterator.
     */
    virtual void reset(StringData document, Options options) = 0;

    /**
     * Moves to the next token in the iterator.
     * Returns false when the iterator reaches end of the document.
     */
    virtual bool moveNext() = 0;

    /**
     * Returns stemmed form, normalized, and lowercased depending on the parameter
     * to the reset method.
     * Returned StringData is valid until next call to moveNext().
     */
    virtual StringData get() const = 0;
};

}  // namespace fts
}  // namespace mongo
