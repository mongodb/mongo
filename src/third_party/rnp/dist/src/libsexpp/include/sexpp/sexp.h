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
 * Original copyright
 *
 * SEXP standard header file: sexp.h
 * Ronald L. Rivest
 * 6/29/1997
 */

#pragma once

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <cinttypes>
#include <climits>
#include <limits>
#include <cctype>
#include <locale>
#include <cstring>
#include <memory>
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>
#include <cassert>

#include "sexp-public.h"
#include "sexp-error.h"

// We are implementing char traits for octet_t with the following restrictions
//  -- limit visibility so that other traits for unsigned char are still possible
//  -- create template specializatio in std workspace (use workspace specialization
//     is not specified and causes issues at least with gcc 4.8

namespace sexp {
using octet_t = uint8_t;
} // namespace sexp

namespace std {

template <> struct char_traits<sexp::octet_t> {
    typedef sexp::octet_t  char_type;
    typedef int            int_type;
    typedef std::streampos pos_type;
    typedef std::streamoff off_type;
    typedef mbstate_t      state_type;

    static void assign(char_type &__c1, const char_type &__c2) noexcept { __c1 = __c2; }

    static constexpr bool eq(const char_type &__c1, const char_type &__c2) noexcept
    {
        return __c1 == __c2;
    }

    static constexpr bool lt(const char_type &__c1, const char_type &__c2) noexcept
    {
        return __c1 < __c2;
    }

    static int compare(const char_type *__s1, const char_type *__s2, size_t __n)
    {
        return memcmp(__s1, __s2, __n);
    }

    static size_t length(const char_type *__s)
    {
        return strlen(reinterpret_cast<const char *>(__s));
    }

    static const char_type *find(const char_type *__s, size_t __n, const char_type &__a)
    {
        return static_cast<const char_type *>(memchr(__s, __a, __n));
    }

    static char_type *move(char_type *__s1, const char_type *__s2, size_t __n)
    {
        return static_cast<char_type *>(memmove(__s1, __s2, __n));
    }

    static char_type *copy(char_type *__s1, const char_type *__s2, size_t __n)
    {
        return static_cast<char_type *>(memcpy(__s1, __s2, __n));
    }

    static char_type *assign(char_type *__s, size_t __n, char_type __a)
    {
        return static_cast<char_type *>(memset(__s, __a, __n));
    }

    static constexpr char_type to_char_type(const int_type &__c) noexcept
    {
        return static_cast<char_type>(__c);
    }

    // To keep both the byte 0xff and the eof symbol 0xffffffff
    // from ending up as 0xffffffff.
    static constexpr int_type to_int_type(const char_type &__c) noexcept
    {
        return static_cast<int_type>(static_cast<unsigned char>(__c));
    }

    static constexpr bool eq_int_type(const int_type &__c1, const int_type &__c2) noexcept
    {
        return __c1 == __c2;
    }

    static constexpr int_type eof() noexcept { return static_cast<int_type>(0xFFFFFFFF); }

    static constexpr int_type not_eof(const int_type &__c) noexcept
    {
        return (__c == eof()) ? 0 : __c;
    }
};
} // namespace std

namespace sexp {
/*
 * SEXP octet_t definitions
 * We maintain some presumable redundancy with ctype
 * However, we do enforce 'C' locale this way
 */

class SEXP_PUBLIC_SYMBOL sexp_char_defs_t {
  protected:
    static const bool          base64digit[256]; /* true if c is base64 digit */
    static const bool          tokenchar[256];   /* true if c can be in a token */
    static const unsigned char values[256][3]; /* values of c as { dec. hex, base64 } digit */
    static std::locale         c_locale;

    static bool is_white_space(int c)
    {
        return c >= 0 && c <= 255 && std::isspace((char) c, c_locale);
    };
    static bool is_dec_digit(int c)
    {
        return c >= 0 && c <= 255 && std::isdigit((char) c, c_locale);
    };
    static bool is_hex_digit(int c)
    {
        return c >= 0 && c <= 255 && std::isxdigit((char) c, c_locale);
    };
    static bool is_base64_digit(int c) { return c >= 0 && c <= 255 && base64digit[c]; };
    static bool is_token_char(int c) { return c >= 0 && c <= 255 && tokenchar[c]; };
    static bool is_alpha(int c)
    {
        return c >= 0 && c <= 255 && std::isalpha((char) c, c_locale);
    };

    /* decvalue(c) is value of c as dec digit */
    static unsigned char decvalue(int c) { return (c >= 0 && c <= 255) ? values[c][0] : 0; };
    /* hexvalue(c) is value of c as a hex digit */
    static unsigned char hexvalue(int c) { return (c >= 0 && c <= 255) ? values[c][1] : 0; };
    /* base64value(c) is value of c as base64 digit */
    static unsigned char base64value(int c)
    {
        return (c >= 0 && c <= 255) ? values[c][2] : 0;
    };
};

class sexp_string_t;
class sexp_list_t;

class sexp_output_stream_t;
class sexp_input_stream_t;

/*
 * SEXP simple string
 */

using octet_traits = std::char_traits<octet_t>;
using octet_string = std::basic_string<octet_t, octet_traits>;

class SEXP_PUBLIC_SYMBOL sexp_simple_string_t : public octet_string, private sexp_char_defs_t {
  public:
    sexp_simple_string_t(void) = default;
    sexp_simple_string_t(const octet_t *dt) : octet_string{dt} {}
    sexp_simple_string_t(const octet_t *bt, size_t ln) : octet_string{bt, ln} {}
    sexp_simple_string_t &append(int c)
    {
        (*this) += (octet_t)(c & 0xFF);
        return *this;
    }
    // Returns length for printing simple string as a token
    size_t advanced_length_token(void) const { return length(); }
    // Returns length for printing simple string as a base64 string
    size_t advanced_length_base64(void) const { return (2 + 4 * ((length() + 2) / 3)); }
    // Returns length for printing simple string ss in quoted-string mode
    size_t advanced_length_quoted(void) const { return (1 + length() + 1); }
    // Returns length for printing simple string ss in hexadecimal mode
    size_t advanced_length_hexadecimal(void) const { return (1 + 2 * length() + 1); }
    size_t advanced_length(sexp_output_stream_t *os) const;

    sexp_output_stream_t *print_canonical_verbatim(sexp_output_stream_t *os) const;
    sexp_output_stream_t *print_advanced(sexp_output_stream_t *os) const;
    sexp_output_stream_t *print_token(sexp_output_stream_t *os) const;
    sexp_output_stream_t *print_quoted(sexp_output_stream_t *os) const;
    sexp_output_stream_t *print_hexadecimal(sexp_output_stream_t *os) const;
    sexp_output_stream_t *print_base64(sexp_output_stream_t *os) const;

    bool can_print_as_quoted_string(void) const;
    bool can_print_as_token(const sexp_output_stream_t *os) const;

    bool operator==(const char *right) const noexcept
    {
        return length() == std::strlen(right) && std::memcmp(data(), right, length()) == 0;
    }

    bool operator!=(const char *right) const noexcept
    {
        return length() != std::strlen(right) || std::memcmp(data(), right, length()) != 0;
    }

    unsigned as_unsigned() const noexcept
    {
        return empty() ? std::numeric_limits<uint32_t>::max() :
                         (unsigned) atoi(reinterpret_cast<const char *>(c_str()));
    }
};

inline bool operator==(const sexp_simple_string_t *left, const std::string &right) noexcept
{
    return *left == right.c_str();
}

inline bool operator!=(const sexp_simple_string_t *left, const std::string &right) noexcept
{
    return *left != right.c_str();
}

/*
 * SEXP object
 */

class SEXP_PUBLIC_SYMBOL sexp_object_t {
  public:
    virtual ~sexp_object_t(){};

    virtual sexp_output_stream_t *print_canonical(sexp_output_stream_t *os) const = 0;
    virtual sexp_output_stream_t *print_advanced(sexp_output_stream_t *os) const;
    virtual size_t                advanced_length(sexp_output_stream_t *os) const = 0;

    virtual sexp_list_t *  sexp_list_view(void) noexcept { return nullptr; }
    virtual sexp_string_t *sexp_string_view(void) noexcept { return nullptr; }
    virtual bool           is_sexp_list(void) const noexcept { return false; }
    virtual bool           is_sexp_string(void) const noexcept { return false; }

    virtual const sexp_list_t *sexp_list_at(
      std::vector<std::shared_ptr<sexp_object_t>>::size_type pos) const noexcept
    {
        return nullptr;
    }
    virtual const sexp_string_t *sexp_string_at(
      std::vector<std::shared_ptr<sexp_object_t>>::size_type pos) const noexcept
    {
        return nullptr;
    }
    virtual const sexp_simple_string_t *sexp_simple_string_at(
      std::vector<std::shared_ptr<sexp_object_t>>::size_type pos) const noexcept
    {
        return nullptr;
    }
    virtual bool     operator==(const char *right) const noexcept { return false; }
    virtual bool     operator!=(const char *right) const noexcept { return true; }
    virtual unsigned as_unsigned() const noexcept
    {
        return std::numeric_limits<uint32_t>::max();
    }
};

/*
 * SEXP string
 */

class SEXP_PUBLIC_SYMBOL sexp_string_t : public sexp_object_t {
  protected:
    bool                 with_presentation_hint;
    sexp_simple_string_t presentation_hint;
    sexp_simple_string_t data_string;

  public:
    sexp_string_t(const octet_t *dt) : with_presentation_hint(false), data_string(dt) {}
    sexp_string_t(const octet_t *bt, size_t ln)
        : with_presentation_hint(false), data_string(bt, ln)
    {
    }
    sexp_string_t(const std::string &str)
        : with_presentation_hint(false),
          data_string(reinterpret_cast<const octet_t *>(str.data()))
    {
    }
    sexp_string_t(void) : with_presentation_hint(false) {}
    sexp_string_t(sexp_input_stream_t *sis) { parse(sis); };

    const bool has_presentation_hint(void) const noexcept { return with_presentation_hint; }
    const sexp_simple_string_t &get_string(void) const noexcept { return data_string; }
    const sexp_simple_string_t &set_string(const sexp_simple_string_t &ss)
    {
        return data_string = ss;
    }
    const sexp_simple_string_t &get_presentation_hint(void) const noexcept
    {
        return presentation_hint;
    }
    const sexp_simple_string_t &set_presentation_hint(const sexp_simple_string_t &ph)
    {
        with_presentation_hint = true;
        return presentation_hint = ph;
    }

    virtual sexp_output_stream_t *print_canonical(sexp_output_stream_t *os) const;
    virtual sexp_output_stream_t *print_advanced(sexp_output_stream_t *os) const;
    virtual size_t                advanced_length(sexp_output_stream_t *os) const;

    virtual sexp_string_t *sexp_string_view(void) noexcept { return this; }
    virtual bool           is_sexp_string(void) const noexcept { return true; }

    virtual bool operator==(const char *right) const noexcept { return data_string == right; }
    virtual bool operator!=(const char *right) const noexcept { return data_string != right; }

    void             parse(sexp_input_stream_t *sis);
    virtual unsigned as_unsigned() const noexcept { return data_string.as_unsigned(); }
};

inline bool operator==(const sexp_string_t *left, const std::string &right) noexcept
{
    return *left == right.c_str();
}

inline bool operator!=(const sexp_string_t *left, const std::string &right) noexcept
{
    return *left != right.c_str();
}

/*
 * SEXP list
 */

class SEXP_PUBLIC_SYMBOL sexp_list_t : public sexp_object_t,
                                       public std::vector<std::shared_ptr<sexp_object_t>> {
  public:
    virtual ~sexp_list_t() {}

    virtual sexp_output_stream_t *print_canonical(sexp_output_stream_t *os) const;
    virtual sexp_output_stream_t *print_advanced(sexp_output_stream_t *os) const;
    virtual size_t                advanced_length(sexp_output_stream_t *os) const;

    virtual sexp_list_t *sexp_list_view(void) noexcept { return this; }
    virtual bool         is_sexp_list(void) const noexcept { return true; }

    virtual const sexp_list_t *sexp_list_at(size_type pos) const noexcept
    {
        return pos < size() ? (*at(pos)).sexp_list_view() : nullptr;
    }
    virtual const sexp_string_t *sexp_string_at(size_type pos) const noexcept
    {
        return pos < size() ? (*at(pos)).sexp_string_view() : nullptr;
    }
    const sexp_simple_string_t *sexp_simple_string_at(size_type pos) const noexcept
    {
        auto s = sexp_string_at(pos);
        return s != nullptr ? &s->get_string() : nullptr;
    }

    void parse(sexp_input_stream_t *sis);
};

/*
    sexp_depth_manager controls maximum allowed nesting of sexp lists
    for sexp_input_stream, sexp_output_stream processing
    One still can create an object with deeper nesting manually
*/

class SEXP_PUBLIC_SYMBOL sexp_depth_manager {
  public:
    static const size_t DEFAULT_MAX_DEPTH = 1024;

  private:
    size_t depth;     /* current depth of nested SEXP lists */
    size_t max_depth; /* maximum allowed depth of nested SEXP lists, 0 if no limit */
  protected:
    sexp_depth_manager(size_t m_depth = DEFAULT_MAX_DEPTH);
    void reset_depth(size_t m_depth);
    void increase_depth(int count = -1);
    void decrease_depth(void);
};

/*
 * SEXP input stream
 */

class SEXP_PUBLIC_SYMBOL sexp_input_stream_t : public sexp_char_defs_t, sexp_depth_manager {
  protected:
    std::istream *input_file;
    uint32_t      byte_size; /* 4 or 6 or 8 == currently scanning mode */
    int           next_char; /* character currently being scanned */
    uint32_t      bits;      /* Bits waiting to be used */
    uint32_t      n_bits;    /* number of such bits waiting to be used */
    int           count;     /* number of 8-bit characters output by get_char */

    virtual int read_char(void);

  public:
    sexp_input_stream_t(std::istream *i,
                        size_t        max_depth = sexp_depth_manager::DEFAULT_MAX_DEPTH);
    virtual ~sexp_input_stream_t() = default;
    sexp_input_stream_t *          set_input(std::istream *i,
                                             size_t max_depth = sexp_depth_manager::DEFAULT_MAX_DEPTH);
    sexp_input_stream_t *          set_byte_size(uint32_t new_byte_size);
    uint32_t                       get_byte_size(void) { return byte_size; }
    sexp_input_stream_t *          get_char(void);
    sexp_input_stream_t *          skip_white_space(void);
    sexp_input_stream_t *          skip_char(int c);
    std::shared_ptr<sexp_object_t> scan_to_eof();
    std::shared_ptr<sexp_object_t> scan_object(void);
    std::shared_ptr<sexp_string_t> scan_string(void);
    std::shared_ptr<sexp_list_t>   scan_list(void);
    sexp_simple_string_t           scan_simple_string(void);
    void                           scan_token(sexp_simple_string_t &ss);
    void     scan_verbatim_string(sexp_simple_string_t &ss, uint32_t length);
    void     scan_quoted_string(sexp_simple_string_t &ss, uint32_t length);
    void     scan_hexadecimal_string(sexp_simple_string_t &ss, uint32_t length);
    void     scan_base64_string(sexp_simple_string_t &ss, uint32_t length);
    uint32_t scan_decimal_string(void);

    int get_next_char(void) const { return next_char; }
    int set_next_char(int c) { return next_char = c; }

    sexp_input_stream_t *open_list(void);
    sexp_input_stream_t *close_list(void);
};

/*
 * SEXP output stream
 */

class SEXP_PUBLIC_SYMBOL sexp_output_stream_t : sexp_depth_manager {
  public:
    const uint32_t default_line_length = 75;
    enum sexp_print_mode {                /* PRINTING MODES */
                           canonical = 1, /* standard for hashing and transmission */
                           base64 = 2,    /* base64 version of canonical */
                           advanced = 3   /* pretty-printed */
    };

  protected:
    std::ostream *  output_file;
    uint32_t        base64_count; /* number of hex or base64 chars printed this region */
    uint32_t        byte_size;    /* 4 or 6 or 8 depending on output mode */
    uint32_t        bits;         /* bits waiting to go out */
    uint32_t        n_bits;       /* number of bits waiting to go out */
    sexp_print_mode mode;         /* base64, advanced, or canonical */
    uint32_t        column;       /* column where next character will go */
    uint32_t        max_column;   /* max usable column, or 0 if no maximum */
    uint32_t        indent;       /* current indentation level (starts at 0) */
  public:
    sexp_output_stream_t(std::ostream *o,
                         size_t        max_depth = sexp_depth_manager::DEFAULT_MAX_DEPTH);
    sexp_output_stream_t *set_output(std::ostream *o,
                                     size_t max_depth = sexp_depth_manager::DEFAULT_MAX_DEPTH);
    sexp_output_stream_t *put_char(int c);                /* output a character */
    sexp_output_stream_t *new_line(sexp_print_mode mode); /* go to next line (and indent) */
    sexp_output_stream_t *var_put_char(int c);
    sexp_output_stream_t *flush(void);
    sexp_output_stream_t *print_decimal(uint64_t n);

    sexp_output_stream_t *change_output_byte_size(int newByteSize, sexp_print_mode mode);

    sexp_output_stream_t *print_canonical(const std::shared_ptr<sexp_object_t> &obj)
    {
        return obj->print_canonical(this);
    }
    sexp_output_stream_t *print_advanced(const std::shared_ptr<sexp_object_t> &obj)
    {
        return obj->print_advanced(this);
    };
    sexp_output_stream_t *print_base64(const std::shared_ptr<sexp_object_t> &obj);
    sexp_output_stream_t *print_canonical(const sexp_simple_string_t *ss)
    {
        return ss->print_canonical_verbatim(this);
    }
    sexp_output_stream_t *print_advanced(const sexp_simple_string_t *ss)
    {
        return ss->print_advanced(this);
    };

    uint32_t              get_byte_size(void) const { return byte_size; }
    uint32_t              get_column(void) const { return column; }
    sexp_output_stream_t *reset_column(void)
    {
        column = 0;
        return this;
    }
    uint32_t              get_max_column(void) const { return max_column; }
    sexp_output_stream_t *set_max_column(uint32_t mc)
    {
        max_column = mc;
        return this;
    }
    sexp_output_stream_t *inc_indent(void)
    {
        ++indent;
        return this;
    }
    sexp_output_stream_t *dec_indent(void)
    {
        --indent;
        return this;
    }

    sexp_output_stream_t *open_list(void)
    {
        put_char('(');
        increase_depth();

        return this;
    }
    sexp_output_stream_t *close_list(void)
    {
        put_char(')')->decrease_depth();
        return this;
    }
    sexp_output_stream_t *var_open_list(void)
    {
        var_put_char('(')->increase_depth();
        return this;
    }
    sexp_output_stream_t *var_close_list(void)
    {
        var_put_char(')')->decrease_depth();
        return this;
    }
};

} // namespace sexp
