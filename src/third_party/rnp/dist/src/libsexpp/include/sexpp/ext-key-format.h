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

#pragma once

#include <map>

#include "sexp-public.h"
#include "sexp.h"

namespace ext_key_format {

void SEXP_PUBLIC_SYMBOL ext_key_error(
  sexp::sexp_exception_t::severity level, const char *msg, size_t c1, size_t c2, int pos);

class ext_key_input_stream_t;

class SEXP_PUBLIC_SYMBOL extended_private_key_t {
  public:
    // Comparison of names is done case insensitively !!!
    struct ci_less {
        // case-independent (ci) compare_less binary function
        bool operator()(const std::string &s1, const std::string &s2) const
        {
            return std::lexicographical_compare(
              s1.begin(), s1.end(), s2.begin(), s2.end(), [](char a, char b) {
                  return std::tolower(a) < std::tolower(b);
              });
        }
    };

    // C++ 11 compatible version (no std::equals)
    static bool iequals(const std::string &a, const std::string &b)
    {
        size_t sz = a.size();
        if (b.size() != sz)
            return false;
        for (size_t i = 0; i < sz; ++i)
            if (tolower(a[i]) != tolower(b[i]))
                return false;
        return true;
    }

    typedef std::multimap<std::string, std::string, ci_less> fields_map_t;

    sexp::sexp_list_t key;
    fields_map_t      fields;

    void parse(ext_key_input_stream_t &is);
};

class SEXP_PUBLIC_SYMBOL ext_key_input_stream_t : public sexp::sexp_input_stream_t {
  private:
    static const bool namechar[256]; /* true if allowed in the name field */

    static bool is_newline_char(int c) { return c == '\r' || c == '\n'; };
    static bool is_namechar(int c) { return ((c >= 0 && c <= 255) && namechar[c]); }

    bool is_scanning_value;
    bool has_key;

    int         skip_line(void);
    virtual int read_char(void);
    std::string scan_name(int c);
    std::string scan_value(void);

  public:
    ext_key_input_stream_t(std::istream *i, size_t md = 0)
        : sexp_input_stream_t(i, md), is_scanning_value(false), has_key(false)
    {
    }
    virtual ~ext_key_input_stream_t() = default;
    void scan(extended_private_key_t &extended_key);
};
} // namespace ext_key_format
