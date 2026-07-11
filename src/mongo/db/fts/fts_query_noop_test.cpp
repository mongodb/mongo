// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/fts/fts_query_noop.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace fts {

TEST(FTSQueryNoop, Parse) {
    FTSQueryNoop q;
    ASSERT_OK(q.parse(TEXT_INDEX_VERSION_INVALID));
}

TEST(FTSQueryNoop, Clone) {
    FTSQueryNoop q;
    q.setQuery("foo");
    q.setLanguage("bar");
    q.setCaseSensitive(true);
    q.setDiacriticSensitive(true);

    auto clone = q.clone();
    ASSERT_EQUALS(clone->getQuery(), q.getQuery());
    ASSERT_EQUALS(clone->getLanguage(), q.getLanguage());
    ASSERT_EQUALS(clone->getCaseSensitive(), q.getCaseSensitive());
    ASSERT_EQUALS(clone->getDiacriticSensitive(), q.getDiacriticSensitive());
}

}  // namespace fts
}  // namespace mongo
