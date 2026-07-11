// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/db/fts/fts_language.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <cstddef>
#include <set>
#include <string>
#include <string_view>

#include <absl/container/node_hash_map.h>

namespace mongo {

namespace fts {

class StopWords {
    StopWords(const StopWords&) = delete;
    StopWords& operator=(const StopWords&) = delete;

public:
    StopWords();
    StopWords(const std::set<std::string>& words);

    bool isStopWord(std::string_view word) const {
        return _words.find(word) != _words.end();
    }

    size_t numStopWords() const {
        return _words.size();
    }

    static const StopWords* getStopWords(const FTSLanguage* language);

private:
    StringMap<bool> _words;  // Used as a set. The values have no meaning.
};
}  // namespace fts
}  // namespace mongo
