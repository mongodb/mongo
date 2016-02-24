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

#include "mongo/db/fts/unicode/codepoints.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace unicode {

const char32_t maxCP = 0x1FFFFF;  // Highest valid codepoint.

/**
 * Above most of the arrays in this class are the UTF-32 character literals that correspond to the
 * codepoints in the array.
 */

TEST(UnicodeCodepoints, Diacritics) {
    // There are no character literals for combining marks.
    const char32_t marks[] = {'^', '`', 0x0301, 0x0339, 0x1AB4, 0x1DC5, 0xA69D};

    // const char32_t not_marks[] = {U'-', U'.', U'\'', U'*', U'm'};
    const char32_t not_marks[] = {0x2D, 0x2E, 0x27, 0x2A, 0x6D};

    for (auto cp : marks) {
        ASSERT(codepointIsDiacritic(cp));
        ASSERT_EQ(codepointRemoveDiacritics(cp), char32_t(0));
    }

    for (auto cp : not_marks) {
        ASSERT(!codepointIsDiacritic(cp));
        ASSERT_NE(codepointRemoveDiacritics(cp), char32_t(0));
    }
}

TEST(UnicodeCodepoints, Delimiters) {
    // const char32_t delimiters[] = {U'-', U'.', U'"', U'¿', U'«'};
    const char32_t delimiters[] = {0x2D, 0x2E, 0x22, 0xBF, 0xAB};
    // const char32_t not_delimiters[] = {U'a', U'ê', U'π', U'Ω', U'å'};
    const char32_t not_delimiters[] = {0x61, 0xEA, 0x3C0, 0x3A9, 0xE5};

    for (auto i = 0; i < 5; ++i) {
        ASSERT(codepointIsDelimiter(delimiters[i], DelimiterListLanguage::kEnglish));
        ASSERT(codepointIsDelimiter(delimiters[i], DelimiterListLanguage::kNotEnglish));
        ASSERT_FALSE(codepointIsDelimiter(not_delimiters[i], DelimiterListLanguage::kEnglish));
        ASSERT_FALSE(codepointIsDelimiter(not_delimiters[i], DelimiterListLanguage::kNotEnglish));
    }

    // Special case for English.
    ASSERT(codepointIsDelimiter(0x27, DelimiterListLanguage::kNotEnglish));
    ASSERT_FALSE(codepointIsDelimiter(0x27, DelimiterListLanguage::kEnglish));
}

TEST(UnicodeCodepoints, RemoveDiacritics) {
    // const char32_t originals[] = {U'á', U'ê', U'ñ', U'å', U'ç'};
    const char32_t originals[] = {0xE1, 0xEA, 0xF1, 0xE5, 0xE7};
    // const char32_t clean[] = {U'a', U'e', U'n', U'a', U'c'};
    const char32_t clean[] = {0x61, 0x65, 0x6E, 0x61, 0x63};

    for (auto i = 0; i < 5; ++i) {
        ASSERT_EQUALS(clean[i], codepointRemoveDiacritics(originals[i]));
    }

    for (char32_t cp = 0; cp <= maxCP; cp++) {
        ASSERT_EQ(codepointRemoveDiacritics(cp) == 0, cp == 0 || codepointIsDiacritic(cp));
    }
}

TEST(UnicodeCodepoints, ToLower) {
    // const char32_t upper[] = {U'Á', U'Ê', U'Ñ', U'Å', U'Ç'};
    const char32_t upper[] = {0xC1, 0xCA, 0xD1, 0xC5, 0xC7};
    // const char32_t lower[] = {U'á', U'ê', U'ñ', U'å', U'ç'};
    const char32_t lower[] = {0xE1, 0xEA, 0xF1, 0xE5, 0xE7};

    for (auto i = 0; i < 5; ++i) {
        ASSERT_EQUALS(lower[i], codepointToLower(upper[i]));
    }
}

TEST(UnicodeCodepoints, ToLowerIsFixedPoint) {
    for (char32_t cp = 0; cp <= maxCP; cp++) {
        ASSERT_EQ(codepointToLower(cp), codepointToLower(codepointToLower(cp)));
    }
}

TEST(UnicodeCodepoints, RemoveDiacriticsIsFixedPoint) {
    for (char32_t cp = 0; cp <= maxCP; cp++) {
        ASSERT_EQ(codepointRemoveDiacritics(cp),
                  codepointRemoveDiacritics(codepointRemoveDiacritics(cp)));
    }
}

}  // namespace unicode
}  // namespace mongo
