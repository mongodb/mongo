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

#include "mongo/db/fts/fts_basic_phrase_matcher.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace fts {

// Case insensitive match.
TEST(FtsBasicPhraseMatcher, CaseInsensitive) {
    std::string str1 = "Lorem ipsum dolor sit amet, consectetur adipiscing elit.";
    std::string find1 = "Consectetur adipiscing";
    std::string nofind1 = "dolor amet";

    std::string str2 = "Duis aute irure dolor in reprehenderit in Voluptate velit esse cillum.";
    std::string find2 = "In Voluptate";
    std::string nofind2 = "dolor velit";

    BasicFTSPhraseMatcher phraseMatcher;
    FTSPhraseMatcher::Options options = FTSPhraseMatcher::kNone;

    ASSERT(phraseMatcher.phraseMatches(find1, str1, options));
    ASSERT(phraseMatcher.phraseMatches(find2, str2, options));

    ASSERT_FALSE(phraseMatcher.phraseMatches(nofind1, str1, options));
    ASSERT_FALSE(phraseMatcher.phraseMatches(nofind2, str2, options));
}

// Case sensitive match.
TEST(FtsBasicPhraseMatcher, CaseSensitive) {
    std::string str1 = "Lorem ipsum dolor sit amet, consectetur adipiscing elit.";
    std::string find1 = "Lorem ipsum";
    std::string nofind1 = "Sit amet";

    std::string str2 = "Duis aute irure dolor in reprehenderit in Voluptate velit esse cillum.";
    std::string find2 = "in Voluptate";
    std::string nofind2 = "Irure dolor";

    BasicFTSPhraseMatcher phraseMatcher;
    FTSPhraseMatcher::Options options = FTSPhraseMatcher::kCaseSensitive;

    ASSERT(phraseMatcher.phraseMatches(find1, str1, options));
    ASSERT(phraseMatcher.phraseMatches(find2, str2, options));

    ASSERT_FALSE(phraseMatcher.phraseMatches(nofind1, str1, options));
    ASSERT_FALSE(phraseMatcher.phraseMatches(nofind2, str2, options));
}

}  // namespace fts
}  // namespace mongo
