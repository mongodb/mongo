// util/base64.h

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

#pragma once

#include <iosfwd>
#include <memory>
#include <string>

#include "mongo/util/assert_util.h"

namespace mongo {
namespace base64 {

class Alphabet {
public:
    Alphabet()
                : encode((unsigned char*)
                         "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                         "abcdefghijklmnopqrstuvwxyz"
                         "0123456789"
                         "+/")
                , decode(new unsigned char[257]) {
        memset(decode.get(), 0, 256);
        for (int i = 0; i < 64; i++) {
            decode[encode[i]] = i;
        }

        test();
    }
    void test() {
        verify(strlen((char*)encode) == 64);
        for (int i = 0; i < 26; i++)
            verify(encode[i] == toupper(encode[i + 26]));
    }

    char e(int x) {
        return encode[x & 0x3f];
    }

private:
    const unsigned char* encode;

public:
    std::unique_ptr<unsigned char[]> decode;
};

extern Alphabet alphabet;


void encode(std::stringstream& ss, const char* data, int size);
std::string encode(const char* data, int size);
std::string encode(const std::string& s);

void decode(std::stringstream& ss, const std::string& s);
std::string decode(const std::string& s);

extern const char* chars;

void testAlphabet();
}
}
