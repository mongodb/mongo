// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/fts/stemmer.h"

#include "mongo/db/fts/fts_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace fts {

namespace {
const FTSLanguage* languageEnglishV2() {
    return &FTSLanguage::make("english", TEXT_INDEX_VERSION_2);
}
const FTSLanguage* languagePorterV1() {
    return &FTSLanguage::make("porter", TEXT_INDEX_VERSION_1);
}
}  // namespace

TEST(English, Stemmer1) {
    Stemmer s(languageEnglishV2());
    ASSERT_EQUALS("run", s.stem("running"));
    ASSERT_EQUALS("Run", s.stem("Running"));
}

TEST(English, Caps) {
    Stemmer s(languagePorterV1());
    ASSERT_EQUALS("unit", s.stem("united"));
    ASSERT_EQUALS("Unite", s.stem("United"));
}
}  // namespace fts
}  // namespace mongo
