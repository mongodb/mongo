// stringutils.h

/*    Copyright 2010 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <ctype.h>

#include <memory>
#include <string>
#include <vector>


#include "mongo/base/string_data.h"

namespace mongo {

// see also mongoutils/str.h - perhaps move these there?
// see also text.h

void splitStringDelim(const std::string& str, std::vector<std::string>* res, char delim);

void joinStringDelim(const std::vector<std::string>& strs, std::string* res, char delim);

inline std::string tolowerString(StringData input) {
    std::string::size_type sz = input.size();

    std::unique_ptr<char[]> line(new char[sz + 1]);
    char* copy = line.get();

    for (std::string::size_type i = 0; i < sz; i++) {
        char c = input[i];
        copy[i] = (char)tolower((int)c);
    }
    copy[sz] = 0;
    return copy;
}

inline std::string toAsciiLowerCase(StringData input) {
    size_t sz = input.size();
    std::unique_ptr<char[]> line(new char[sz + 1]);
    char* res = line.get();
    for (size_t i = 0; i < sz; i++) {
        char c = input[i];
        if (c >= 'A' && c <= 'Z') {
            res[i] = c + 32;
        } else {
            res[i] = c;
        }
    }
    res[sz] = 0;
    return res;
}

/** Functor for combining lexical and numeric comparisons. */
class LexNumCmp {
public:
    /** @param lexOnly - compare all characters lexically, including digits. */
    LexNumCmp(bool lexOnly);
    /**
     * Non numeric characters are compared lexicographically; numeric substrings
     * are compared numerically; dots separate ordered comparable subunits.
     * For convenience, character 255 is greater than anything else.
     * @param lexOnly - compare all characters lexically, including digits.
     */
    static int cmp(StringData s1, StringData s2, bool lexOnly);
    int cmp(StringData s1, StringData s2) const;
    bool operator()(StringData s1, StringData s2) const;

private:
    bool _lexOnly;
};

// TODO: Sane-ify core std::string functionality
// For now, this needs to be near the LexNumCmp or else
int versionCmp(const StringData rhs, const StringData lhs);

}  // namespace mongo
