// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/fts/stop_words.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <set>
#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>

namespace mongo {

namespace fts {

void loadStopWordMap(StringMap<std::set<std::string>>* m);

namespace {
StringMap<std::shared_ptr<StopWords>> StopWordsMap;
StopWords empty;
}  // namespace


StopWords::StopWords() {}

StopWords::StopWords(const std::set<std::string>& words) {
    for (std::set<std::string>::const_iterator i = words.begin(); i != words.end(); ++i)
        _words[*i] = true;
}

const StopWords* StopWords::getStopWords(const FTSLanguage* language) {
    auto i = StopWordsMap.find(language->str());
    if (i == StopWordsMap.end())
        return &empty;
    return i->second.get();
}


MONGO_INITIALIZER(StopWords)(InitializerContext* context) {
    StringMap<std::set<std::string>> raw;
    loadStopWordMap(&raw);
    for (StringMap<std::set<std::string>>::const_iterator i = raw.begin(); i != raw.end(); ++i) {
        StopWordsMap[i->first] = std::make_shared<StopWords>(i->second);
    }
}
}  // namespace fts
}  // namespace mongo
