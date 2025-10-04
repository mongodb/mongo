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

#include "mongo/base/string_data.h"
#include "mongo/db/fts/fts_language.h"
#include "mongo/db/fts/fts_tokenizer.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {
namespace fts {

std::vector<std::string> tokenizeString(const char* str, const char* language) {
    // To retrieve the FTSBasicTokenizer, use TEXT_INDEX_VERSION_2
    auto tokenizer = FTSLanguage::make(language, TEXT_INDEX_VERSION_2).createTokenizer();

    tokenizer->reset(str, FTSTokenizer::kNone);

    std::vector<std::string> terms;

    while (tokenizer->moveNext()) {
        terms.push_back(std::string{tokenizer->get()});
    }

    return terms;
}

// Ensure punctuation is filtered out of the indexed document
// and the 's is not separated
TEST(FtsBasicTokenizer, English) {
    std::vector<std::string> terms = tokenizeString("Do you see Mark's dog running?", "english");

    ASSERT_EQUALS(6U, terms.size());
    ASSERT_EQUALS("do", terms[0]);
    ASSERT_EQUALS("you", terms[1]);
    ASSERT_EQUALS("see", terms[2]);
    ASSERT_EQUALS("mark", terms[3]);
    ASSERT_EQUALS("dog", terms[4]);
    ASSERT_EQUALS("run", terms[5]);
}

// Ensure punctuation is filtered out of the indexed document
// and the 's is separated
TEST(FtsBasicTokenizer, French) {
    std::vector<std::string> terms = tokenizeString("Do you see Mark's dog running?", "french");

    ASSERT_EQUALS(7U, terms.size());
    ASSERT_EQUALS("do", terms[0]);
    ASSERT_EQUALS("you", terms[1]);
    ASSERT_EQUALS("se", terms[2]);
    ASSERT_EQUALS("mark", terms[3]);
    ASSERT_EQUALS("s", terms[4]);
    ASSERT_EQUALS("dog", terms[5]);
    ASSERT_EQUALS("running", terms[6]);
}

}  // namespace fts
}  // namespace mongo
