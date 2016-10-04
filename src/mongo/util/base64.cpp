// util/base64.cpp


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

#include "mongo/util/base64.h"

#include <sstream>

namespace mongo {

using std::string;
using std::stringstream;

namespace base64 {

Alphabet alphabet;

void encode(stringstream& ss, const char* data, int size) {
    for (int i = 0; i < size; i += 3) {
        int left = size - i;
        const unsigned char* start = (const unsigned char*)data + i;

        // byte 0
        ss << alphabet.e(start[0] >> 2);

        // byte 1
        unsigned char temp = (start[0] << 4);
        if (left == 1) {
            ss << alphabet.e(temp);
            break;
        }
        temp |= ((start[1] >> 4) & 0xF);
        ss << alphabet.e(temp);

        // byte 2
        temp = (start[1] & 0xF) << 2;
        if (left == 2) {
            ss << alphabet.e(temp);
            break;
        }
        temp |= ((start[2] >> 6) & 0x3);
        ss << alphabet.e(temp);

        // byte 3
        ss << alphabet.e(start[2] & 0x3f);
    }

    int mod = size % 3;
    if (mod == 1) {
        ss << "==";
    } else if (mod == 2) {
        ss << "=";
    }
}


string encode(const char* data, int size) {
    stringstream ss;
    encode(ss, data, size);
    return ss.str();
}

string encode(const string& s) {
    return encode(s.c_str(), s.size());
}


void decode(stringstream& ss, const string& s) {
    uassert(10270, "invalid base64", s.size() % 4 == 0);
    const unsigned char* data = (const unsigned char*)s.c_str();
    int size = s.size();

    unsigned char buf[3];
    for (int i = 0; i < size; i += 4) {
        const unsigned char* start = data + i;
        buf[0] =
            ((alphabet.decode[start[0]] << 2) & 0xFC) | ((alphabet.decode[start[1]] >> 4) & 0x3);
        buf[1] =
            ((alphabet.decode[start[1]] << 4) & 0xF0) | ((alphabet.decode[start[2]] >> 2) & 0xF);
        buf[2] = ((alphabet.decode[start[2]] << 6) & 0xC0) | ((alphabet.decode[start[3]] & 0x3F));

        int len = 3;
        if (start[3] == '=') {
            len = 2;
            if (start[2] == '=') {
                len = 1;
            }
        }
        ss.write((const char*)buf, len);
    }
}

string decode(const string& s) {
    stringstream ss;
    decode(ss, s);
    return ss.str();
}

const char* chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/=";
}
}
