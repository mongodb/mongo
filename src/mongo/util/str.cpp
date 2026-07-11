// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/str.h"

#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/ctype.h"
#include "mongo/util/hex.h"

#include <cstdio>
#include <memory>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::str {

void splitStringDelim(const std::string& str, std::vector<std::string>* res, char delim) {
    if (str.empty())
        return;

    size_t beg = 0;
    size_t pos = str.find(delim);
    while (pos != str.npos) {
        res->push_back(str.substr(beg, pos - beg));
        beg = ++pos;
        pos = str.find(delim, beg);
    }
    res->push_back(str.substr(beg));
}

void joinStringDelim(const std::vector<std::string>& strs, std::string* res, char delim) {
    for (auto it = strs.begin(); it != strs.end(); ++it) {
        if (it != strs.begin())
            res->push_back(delim);
        res->append(*it);
    }
}

LexNumCmp::LexNumCmp(bool lexOnly) : _lexOnly(lexOnly) {}

int LexNumCmp::cmp(std::string_view sd1, std::string_view sd2, bool lexOnly) {
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
            bool n1 = ctype::isDigit(sd1[s1]);
            bool n2 = ctype::isDigit(sd2[s2]);

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

                while (e1 < sd1.size() && ctype::isDigit(sd1[e1]))
                    e1++;
                while (e2 < sd2.size() && ctype::isDigit(sd2[e2]))
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
                // if the lengths of digits are equal, just memcmp
                else {
                    result = memcmp(sd1.data() + s1, sd2.data() + s2, len1);
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

int LexNumCmp::cmp(std::string_view s1, std::string_view s2) const {
    return cmp(s1, s2, _lexOnly);
}
bool LexNumCmp::operator()(std::string_view s1, std::string_view s2) const {
    return cmp(s1, s2) < 0;
}

std::string escape(std::string_view sd, bool escape_slash) {
    StringBuilder ret;
    ret.reset(sd.size());
    for (const auto& c : sd) {
        switch (c) {
            case '"':
                ret << "\\\"";
                break;
            case '\\':
                ret << "\\\\";
                break;
            case '/':
                ret << (escape_slash ? "\\/" : "/");
                break;
            case '\b':
                ret << "\\b";
                break;
            case '\f':
                ret << "\\f";
                break;
            case '\n':
                ret << "\\n";
                break;
            case '\r':
                ret << "\\r";
                break;
            case '\t':
                ret << "\\t";
                break;
            default:
                if (c >= 0 && c <= 0x1f) {
                    // For c < 0x7f, ASCII value == Unicode code point.
                    ret << "\\u00" << hexblob::encodeLower(&c, 1);
                } else {
                    ret << c;
                }
        }
    }
    return ret.str();
}

boost::optional<size_t> parseUnsignedBase10Integer(std::string_view fieldName) {
    // Do not accept positions like '-4' or '+4'
    if (!ctype::isDigit(fieldName[0])) {
        return boost::none;
    }
    unsigned int index;
    auto status = NumberParser().base(10)(fieldName, &index);
    if (status.isOK()) {
        return static_cast<size_t>(index);
    }
    return boost::none;
}

std::string convertDoubleToString(double d, int prec) {
    char buffer[StringBuilder::MONGO_DBL_SIZE];
    int z = snprintf(buffer, sizeof(buffer), "%.*g", prec, d);
    invariant(z >= 0);
    return std::string(buffer);
}

}  // namespace mongo::str
