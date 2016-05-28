/**
 * Copyright (C) 2012 10gen Inc.
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
