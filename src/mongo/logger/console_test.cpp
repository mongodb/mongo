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

/**
 * Unit tests of the unittest framework itself.
 */

#include "mongo/logger/console.h"
#include "mongo/unittest/unittest.h"
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>

namespace {

using std::cout;
using std::endl;
using std::ostream;
using std::string;
using std::stringstream;

TEST(ConsoleTest, testUtf8) {
    // this constant should match ConsoleStreamBuffer::bufferSize in console.cpp
    const size_t bufferSize = 1024U;

    mongo::Console console;
    ostream& out = console.out();

    // example unicode code points are from:
    //     http://en.wikipedia.org/wiki/UTF-8#Examples
    struct {
        size_t length;
        const char* utf8CodePoint;
    } data[] = {
        {2U, "\xc2\xa2"},          // U+0024
        {3U, "\xe2\x82\xac"},      // U+20AC
        {4U, "\xf0\xa4\xad\xa2"},  // U+24B62
        {0, "unused"},
    };

    // generate strings with unicode point located near end of buffer
    // to see how console stream handles incomplete unicode multi-byte sequences

    // to see these multibtye code points in the Windows terminal, set the console font
    // to Lucida. Currently the 4-byte sequence used in the test data is still not being
    // displayed properly.

    for (int i = 0; data[i].length > 0; i++) {
        for (size_t prefixLength = 0; prefixLength <= data[i].length; prefixLength++) {
            stringstream descriptionStream;
            descriptionStream << "ConsoleTest::testUtf8 - checking handling of "
                              << "multi-byte sequence. This line contains " << prefixLength
                              << " of " << data[i].length << "-byte sequence "
                              << "before the end of the console's internal " << bufferSize
                              << "-byte buffer";
            string description = descriptionStream.str();
            size_t padLength = bufferSize - prefixLength;
            string padding(padLength - description.length(), '.');
            string line = description + padding + data[i].utf8CodePoint;
            out << line << endl;
        }
    }

    // check if out is std::cout
    out << "ConsoleTest::testUtf8 - Console::out() is using "
        << (out.rdbuf() == cout.rdbuf() ? "std::cout" : "custom output stream") << endl;
}

}  // namespace
