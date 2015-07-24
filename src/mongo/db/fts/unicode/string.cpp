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

#include "mongo/db/fts/unicode/string.h"

#include <algorithm>

#include "mongo/shell/linenoise_utf8.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace unicode {

using linenoise_utf8::copyString32to8;
using linenoise_utf8::copyString8to32;

using std::u32string;

String::String(const StringData utf8_src) {
    setData(utf8_src);
}

void String::resetData(const StringData utf8_src) {
    setData(utf8_src);
}

void String::setData(const StringData utf8_src) {
    // _data is the target, resize it so that it's guaranteed to fit all of the input characters,
    // plus a null character if there isn't one.
    _data.resize(utf8_src.size() + 1);

    int result = 0;
    size_t resultSize = 0;

    // Although utf8_src.rawData() is not guaranteed to be null-terminated, copyString8to32 won't
    // access bad memory because it is limited by the size of its output buffer, which is set to the
    // size of utf8_src.
    copyString8to32(&_data[0],
                    reinterpret_cast<const unsigned char*>(&utf8_src.rawData()[0]),
                    _data.size(),
                    resultSize,
                    result);

    uassert(28755, "text contains invalid UTF-8", result == 0);

    // Resize _data so it is only as big as what it contains.
    _data.resize(resultSize);
}

String::String(u32string&& src) : _data(std::move(src)) {}

std::string String::toString() const {
    // output is the target, resize it so that it's guaranteed to fit all of the input characters,
    // plus a null character if there isn't one.
    std::string output(_data.size() * 4 + 1, '\0');
    size_t resultSize =
        copyString32to8(reinterpret_cast<unsigned char*>(&output[0]), &_data[0], output.size());

    // Resize output so it is only as large as what it contains.
    output.resize(resultSize);
    return output;
}

size_t String::size() const {
    return _data.size();
}

const char32_t& String::operator[](int i) const {
    return _data[i];
}

String String::substr(size_t pos, size_t len) const {
    return String(_data.substr(pos, len));
}

String String::toLower(CaseFoldMode mode) const {
    u32string newdata(_data.size(), 0);
    auto index = 0;
    for (auto codepoint : _data) {
        newdata[index++] = codepointToLower(codepoint, mode);
    }

    return String(std::move(newdata));
}

String String::removeDiacritics() const {
    u32string newdata(_data.size(), 0);
    auto index = 0;
    for (auto codepoint : _data) {
        if (!codepointIsDiacritic(codepoint)) {
            newdata[index++] = codepointRemoveDiacritics(codepoint);
        }
    }

    newdata.resize(index);
    return String(std::move(newdata));
}

bool String::substrMatch(const String& str,
                         const String& find,
                         SubstrMatchOptions options,
                         CaseFoldMode cfMode) {
    // In Turkish, lowercasing needs to be applied first because the letter İ has a different case
    // folding mapping than the letter I, but removing diacritics removes the dot from İ.
    if (cfMode == CaseFoldMode::kTurkish) {
        String cleanStr = str.toLower(cfMode);
        String cleanFind = find.toLower(cfMode);
        return substrMatch(cleanStr, cleanFind, options | kCaseSensitive, CaseFoldMode::kNormal);
    }

    if (options & kDiacriticSensitive) {
        if (options & kCaseSensitive) {
            // Case sensitive and diacritic sensitive.
            return std::search(str._data.cbegin(),
                               str._data.cend(),
                               find._data.cbegin(),
                               find._data.cend(),
                               [&](char32_t c1, char32_t c2) { return (c1 == c2); }) !=
                str._data.cend();
        }

        // Case insensitive and diacritic sensitive.
        return std::search(str._data.cbegin(),
                           str._data.cend(),
                           find._data.cbegin(),
                           find._data.cend(),
                           [&](char32_t c1, char32_t c2) {
                               return (codepointToLower(c1, cfMode) ==
                                       codepointToLower(c2, cfMode));
                           }) != str._data.cend();
    }

    String cleanStr = str.removeDiacritics();
    String cleanFind = find.removeDiacritics();

    return substrMatch(cleanStr, cleanFind, options | kDiacriticSensitive, cfMode);
}

}  // namespace unicode
}  // namespace mongo
