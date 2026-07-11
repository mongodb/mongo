// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/fts/stop_words.h"

#include "mongo/db/fts/fts_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace fts {

TEST(English, Basic1) {
    const FTSLanguage* lang = &FTSLanguage::make("english", TEXT_INDEX_VERSION_2);
    const StopWords* englishStopWords = StopWords::getStopWords(lang);
    ASSERT(englishStopWords->isStopWord("the"));
    ASSERT(!englishStopWords->isStopWord("computer"));
}
}  // namespace fts
}  // namespace mongo
