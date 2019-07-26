/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <set>
#include <string>

#include "mongo/db/fts/stop_words.h"

#include "mongo/base/init.h"
#include "mongo/util/string_map.h"

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
        StopWordsMap[i->first].reset(new StopWords(i->second));
    }
    return Status::OK();
}
}  // namespace fts
}  // namespace mongo
