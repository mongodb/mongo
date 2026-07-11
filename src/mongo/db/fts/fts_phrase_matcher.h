// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace mongo {
namespace fts {

/**
 * An interface for substring matching routines.
 */
class FTSPhraseMatcher {
public:
    virtual ~FTSPhraseMatcher() = default;

    using Options = uint8_t;

    /**
     * Use no options.
     */
    static const int kNone = 0;

    /**
     * Lowercase strings as part of phrase matching.
     */
    static const int kCaseSensitive = 1 << 0;

    /**
     * Remove diacritics (thus ignoring them) as part of phrase matching.
     */
    static const int kDiacriticSensitive = 1 << 1;

    /**
     * Does the string 'phrase' occur in the string 'haystack'?
     */
    virtual bool phraseMatches(std::string_view phrase,
                               std::string_view haystack,
                               Options options) const = 0;
};

}  // namespace fts
}  // namespace mongo
