// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/util/modules.h"

#include <cstdint>
#include <string_view>

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
    virtual void reset(std::string_view document, Options options) = 0;

    /**
     * Moves to the next token in the iterator.
     * Returns false when the iterator reaches end of the document.
     */
    virtual bool moveNext() = 0;

    /**
     * Returns stemmed form, normalized, and lowercased depending on the parameter
     * to the reset method.
     * Returned std::string_view is valid until next call to moveNext().
     */
    virtual std::string_view get() const = 0;
};

}  // namespace fts
}  // namespace mongo
