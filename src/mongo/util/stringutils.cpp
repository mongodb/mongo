// stringutils.cpp

/*    Copyright 2009 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/util/stringutils.h"

namespace mongo {

using std::string;
using std::vector;

void splitStringDelim(const string& str, vector<string>* res, char delim) {
    if (str.empty())
        return;

    size_t beg = 0;
    size_t pos = str.find(delim);
    while (pos != string::npos) {
        res->push_back(str.substr(beg, pos - beg));
        beg = ++pos;
        pos = str.find(delim, beg);
    }
    res->push_back(str.substr(beg));
}

void joinStringDelim(const vector<string>& strs, string* res, char delim) {
    for (vector<string>::const_iterator it = strs.begin(); it != strs.end(); ++it) {
        if (it != strs.begin())
            res->push_back(delim);
        res->append(*it);
    }
}

LexNumCmp::LexNumCmp(bool lexOnly) : _lexOnly(lexOnly) {}

int LexNumCmp::cmp(StringData sd1, StringData sd2, bool lexOnly) {
    bool startWord = true;

    size_t s1 = 0;
    size_t s2 = 0;

    while (s1 < sd1.size() && s2 < sd2.size()) {
        bool d1 = (sd1[s1] == '.');
        bool d2 = (sd2[s2] == '.');
        if (d1 && !d2)
            return -1;
        if (d2 && !d1)
            return 1;
        if (d1 && d2) {
            ++s1;
            ++s2;
            startWord = true;
            continue;
        }

        bool p1 = (sd1[s1] == (char)255);
        bool p2 = (sd2[s2] == (char)255);

        if (p1 && !p2)
            return 1;
        if (p2 && !p1)
            return -1;

        if (!lexOnly) {
            bool n1 = isdigit(sd1[s1]);
            bool n2 = isdigit(sd2[s2]);

            if (n1 && n2) {
                // get rid of leading 0s
                if (startWord) {
                    while (s1 < sd1.size() && sd1[s1] == '0')
                        s1++;
                    while (s2 < sd2.size() && sd2[s2] == '0')
                        s2++;
                }

                size_t e1 = s1;
                size_t e2 = s2;

                while (e1 < sd1.size() && isdigit(sd1[e1]))
                    e1++;
                while (e2 < sd2.size() && isdigit(sd2[e2]))
                    e2++;

                size_t len1 = e1 - s1;
                size_t len2 = e2 - s2;

                int result;
                // if one is longer than the other, return
                if (len1 > len2) {
                    return 1;
                } else if (len2 > len1) {
                    return -1;
                }
                // if the lengths are equal, just strcmp
                else {
                    result = strncmp(sd1.rawData() + s1, sd2.rawData() + s2, len1);
                    if (result)
                        return (result > 0) ? 1 : -1;
                }

                // otherwise, the numbers are equal
                s1 = e1;
                s2 = e2;
                startWord = false;
                continue;
            }

            if (n1)
                return 1;

            if (n2)
                return -1;
        }

        if (sd1[s1] > sd2[s2])
            return 1;

        if (sd2[s2] > sd1[s1])
            return -1;

        s1++;
        s2++;
        startWord = false;
    }

    if (s1 < sd1.size() && sd1[s1])
        return 1;
    if (s2 < sd2.size() && sd2[s2])
        return -1;
    return 0;
}

int LexNumCmp::cmp(StringData s1, StringData s2) const {
    return cmp(s1, s2, _lexOnly);
}
bool LexNumCmp::operator()(StringData s1, StringData s2) const {
    return cmp(s1, s2) < 0;
}

int versionCmp(const StringData rhs, const StringData lhs) {
    if (rhs == lhs)
        return 0;

    // handle "1.2.3-" and "1.2.3-pre"
    if (rhs.size() < lhs.size()) {
        if (strncmp(rhs.rawData(), lhs.rawData(), rhs.size()) == 0 && lhs[rhs.size()] == '-')
            return +1;
    } else if (rhs.size() > lhs.size()) {
        if (strncmp(rhs.rawData(), lhs.rawData(), lhs.size()) == 0 && rhs[lhs.size()] == '-')
            return -1;
    }

    return LexNumCmp::cmp(rhs, lhs, false);
}

}  // namespace mongo
