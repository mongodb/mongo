/**
 *
 * Copyright 2021-2023 Ribose Inc. (https://www.ribose.com)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <algorithm>
#include <fstream>

#include "sexp-tests.h"

bool compare_binary_files(const std::string &filename1, const std::string &filename2)
{
    bool          res = false;
    std::ifstream file1(filename1, std::ifstream::ate | std::ifstream::binary);
    std::ifstream file2(filename2, std::ifstream::ate | std::ifstream::binary);

    if (file1.tellg() == file2.tellg()) { // otherwise different file size
        file1.seekg(0);
        file2.seekg(0);

        std::istreambuf_iterator<char> begin1(file1);
        std::istreambuf_iterator<char> begin2(file2);

        res = std::equal(begin1, std::istreambuf_iterator<char>(), begin2);
    }

    return res;
}

bool compare_binary_files(const std::string &filename1, std::istream &file2)
{
    std::ifstream file1(filename1, std::ifstream::binary);

    std::istreambuf_iterator<char> begin1(file1);
    std::istreambuf_iterator<char> begin2(file2);
    return std::equal(begin1, std::istreambuf_iterator<char>(), begin2);
}

std::istream &safe_get_line(std::istream &is, std::string &t)
{
    t.clear();

    // The characters in the stream are read one-by-one using a std::streambuf.
    // That is faster than reading them one-by-one using the std::istream.
    // Code that uses streambuf this way must be guarded by a sentry object.
    // The sentry object performs various tasks,
    // such as thread synchronization and updating the stream state.

    std::istream::sentry se(is, true);
    std::streambuf *     sb = is.rdbuf();

    for (;;) {
        int c = sb->sbumpc();
        switch (c) {
        case '\n':
            return is;
        case '\r':
            if (sb->sgetc() == '\n')
                sb->sbumpc();
            return is;
        case std::streambuf::traits_type::eof():
            // Also handle the case when the last line has no line ending
            if (t.empty())
                is.setstate(std::ios::eofbit);
            return is;
        default:
            t += (char) c;
        }
    }
}

bool compare_text_files(const std::string &filename1, const std::string &filename2)
{
    bool          res = true;
    std::ifstream file1(filename1, std::ifstream::binary);
    std::ifstream file2(filename2, std::ifstream::binary);
    std::string   s1, s2;

    while (res) {
        if (file1.eof() && file2.eof())
            break;
        safe_get_line(file1, s1);
        safe_get_line(file2, s2);
        if (s1 != s2)
            res = false;
    }

    return res;
}

bool compare_text_files(const std::string &filename1, std::istream &file2)
{
    bool          res = true;
    std::ifstream file1(filename1, std::ifstream::binary);
    std::string   s1, s2;
    file2.seekg(0);

    while (res) {
        if (file1.eof() && file2.eof())
            break;
        safe_get_line(file1, s1);
        safe_get_line(file2, s2);
        if (s1 != s2)
            res = false;
    }

    return res;
}
