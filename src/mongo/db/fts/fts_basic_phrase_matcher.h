// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/fts/fts_phrase_matcher.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

namespace mongo {
namespace fts {

/**
 * A phrase matcher that looks for exact substring matches with optional ASCII-aware case
 * insensitivity. This phrase matcher does not implement the kDiacriticSensitive match option. All
 * operations are inherently diacritic sensitive.
 */
class BasicFTSPhraseMatcher final : public FTSPhraseMatcher {
    BasicFTSPhraseMatcher(const BasicFTSPhraseMatcher&) = delete;
    BasicFTSPhraseMatcher& operator=(const BasicFTSPhraseMatcher&) = delete;

public:
    BasicFTSPhraseMatcher() = default;

    bool phraseMatches(std::string_view phrase,
                       std::string_view haystack,
                       Options options) const override;
};

}  // namespace fts
}  // namespace mongo
