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

#include "sexpp/ext-key-format.h"

using namespace sexp;

namespace ext_key_format {

void ext_key_error(
  sexp_exception_t::severity level, const char *msg, size_t c1, size_t c2, int pos)
{
    char                       tmp[256];
    sexp_exception_t::severity l = (sexp_exception_t::severity) level;
    snprintf(tmp, sizeof(tmp) / sizeof(tmp[0]), msg, c1, c2);
    if (sexp_exception_t::shall_throw(l))
        throw sexp_exception_t(tmp, l, pos, "EXTENDED KEY FORMAT");
    if (sexp_exception_t::is_interactive()) {
        std::cout.flush() << std::endl
                          << "*** "
                          << sexp_exception_t::format("EXTENDED KEY FORMAT", tmp, l, pos)
                          << " ***" << std::endl;
    }
}

// Valid characters are all ASCII letters, numbers and the hyphen.
// true if allowed in the name field
const bool ext_key_input_stream_t::namechar[256] = {
  /* 0x00   */ false, /* 0x01   */ false, /* 0x02   */ false,
  /* 0x03   */ false, /* 0x04   */ false, /* 0x05   */ false,
  /* 0x06   */ false, /* 0x07   */ false, /* 0x08   */ false,
  /* 0x09   */ false, /* 0x0a   */ false, /* 0x0b   */ false,
  /* 0x0c   */ false, /* 0x0d   */ false, /* 0x0e   */ false,
  /* 0x0f   */ false, /* 0x10   */ false, /* 0x11   */ false,
  /* 0x12   */ false, /* 0x13   */ false, /* 0x14   */ false,
  /* 0x15   */ false, /* 0x16   */ false, /* 0x17   */ false,
  /* 0x18   */ false, /* 0x19   */ false, /* 0x1a   */ false,
  /* 0x1b   */ false, /* 0x1c   */ false, /* 0x1d   */ false,
  /* 0x1e   */ false, /* 0x1f   */ false, /* 0x20   */ false,
  /* 0x21 ! */ false, /* 0x22 " */ false, /* 0x23 # */ false,
  /* 0x24 $ */ false, /* 0x25 % */ false, /* 0x26 & */ false,
  /* 0x27 ' */ false, /* 0x28 ( */ false, /* 0x29 ) */ false,
  /* 0x2a * */ false, /* 0x2b + */ false, /* 0x2c , */ false,
  /* 0x2d - */ true,  /* 0x2e . */ false, /* 0x2f / */ false,
  /* 0x30 0 */ true,  /* 0x31 1 */ true,  /* 0x32 2 */ true,
  /* 0x33 3 */ true,  /* 0x34 4 */ true,  /* 0x35 5 */ true,
  /* 0x36 6 */ true,  /* 0x37 7 */ true,  /* 0x38 8 */ true,
  /* 0x39 9 */ true,  /* 0x3a : */ false, /* 0x3b ; */ false,
  /* 0x3c < */ false, /* 0x3d = */ false, /* 0x3e > */ false,
  /* 0x3f ? */ false, /* 0x40 @ */ false, /* 0x41 A */ true,
  /* 0x42 B */ true,  /* 0x43 C */ true,  /* 0x44 D */ true,
  /* 0x45 E */ true,  /* 0x46 F */ true,  /* 0x47 G */ true,
  /* 0x48 H */ true,  /* 0x49 I */ true,  /* 0x4a J */ true,
  /* 0x4b K */ true,  /* 0x4c L */ true,  /* 0x4d M */ true,
  /* 0x4e N */ true,  /* 0x4f O */ true,  /* 0x50 P */ true,
  /* 0x51 Q */ true,  /* 0x52 R */ true,  /* 0x53 S */ true,
  /* 0x54 T */ true,  /* 0x55 U */ true,  /* 0x56 V */ true,
  /* 0x57 W */ true,  /* 0x58 X */ true,  /* 0x59 Y */ true,
  /* 0x5a Z */ true,  /* 0x5b [ */ false, /* 0x5c \ */ false,
  /* 0x5d ] */ false, /* 0x5e ^ */ false, /* 0x5f _ */ false,
  /* 0x60 ` */ false, /* 0x61 a */ true,  /* 0x62 b */ true,
  /* 0x63 c */ true,  /* 0x64 d */ true,  /* 0x65 e */ true,
  /* 0x66 f */ true,  /* 0x67 g */ true,  /* 0x68 h */ true,
  /* 0x69 i */ true,  /* 0x6a j */ true,  /* 0x6b k */ true,
  /* 0x6c l */ true,  /* 0x6d m */ true,  /* 0x6e n */ true,
  /* 0x6f o */ true,  /* 0x70 p */ true,  /* 0x71 q */ true,
  /* 0x72 r */ true,  /* 0x73 s */ true,  /* 0x74 t */ true,
  /* 0x75 u */ true,  /* 0x76 v */ true,  /* 0x77 w */ true,
  /* 0x78 x */ true,  /* 0x79 y */ true,  /* 0x7a z */ true,
  /* 0x7b { */ false, /* 0x7c | */ false, /* 0x7d } */ false,
  /* 0x7e ~ */ false, /* 0x7f   */ false, /* 0x80   */ false,
  /* 0x81   */ false, /* 0x82   */ false, /* 0x83   */ false,
  /* 0x84   */ false, /* 0x85   */ false, /* 0x86   */ false,
  /* 0x87   */ false, /* 0x88   */ false, /* 0x89   */ false,
  /* 0x8a   */ false, /* 0x8b   */ false, /* 0x8c   */ false,
  /* 0x8d   */ false, /* 0x8e   */ false, /* 0x8f   */ false,
  /* 0x90   */ false, /* 0x91   */ false, /* 0x92   */ false,
  /* 0x93   */ false, /* 0x94   */ false, /* 0x95   */ false,
  /* 0x96   */ false, /* 0x97   */ false, /* 0x98   */ false,
  /* 0x99   */ false, /* 0x9a   */ false, /* 0x9b   */ false,
  /* 0x9c   */ false, /* 0x9d   */ false, /* 0x9e   */ false,
  /* 0x9f   */ false, /* 0xa0   */ false, /* 0xa1   */ false,
  /* 0xa2   */ false, /* 0xa3   */ false, /* 0xa4   */ false,
  /* 0xa5   */ false, /* 0xa6   */ false, /* 0xa7   */ false,
  /* 0xa8   */ false, /* 0xa9   */ false, /* 0xaa   */ false,
  /* 0xab   */ false, /* 0xac   */ false, /* 0xad   */ false,
  /* 0xae   */ false, /* 0xaf   */ false, /* 0xb0   */ false,
  /* 0xb1   */ false, /* 0xb2   */ false, /* 0xb3   */ false,
  /* 0xb4   */ false, /* 0xb5   */ false, /* 0xb6   */ false,
  /* 0xb7   */ false, /* 0xb8   */ false, /* 0xb9   */ false,
  /* 0xba   */ false, /* 0xbb   */ false, /* 0xbc   */ false,
  /* 0xbd   */ false, /* 0xbe   */ false, /* 0xbf   */ false,
  /* 0xc0   */ false, /* 0xc1   */ false, /* 0xc2   */ false,
  /* 0xc3   */ false, /* 0xc4   */ false, /* 0xc5   */ false,
  /* 0xc6   */ false, /* 0xc7   */ false, /* 0xc8   */ false,
  /* 0xc9   */ false, /* 0xca   */ false, /* 0xcb   */ false,
  /* 0xcc   */ false, /* 0xcd   */ false, /* 0xce   */ false,
  /* 0xcf   */ false, /* 0xd0   */ false, /* 0xd1   */ false,
  /* 0xd2   */ false, /* 0xd3   */ false, /* 0xd4   */ false,
  /* 0xd5   */ false, /* 0xd6   */ false, /* 0xd7   */ false,
  /* 0xd8   */ false, /* 0xd9   */ false, /* 0xda   */ false,
  /* 0xdb   */ false, /* 0xdc   */ false, /* 0xdd   */ false,
  /* 0xde   */ false, /* 0xdf   */ false, /* 0xe0   */ false,
  /* 0xe1   */ false, /* 0xe2   */ false, /* 0xe3   */ false,
  /* 0xe4   */ false, /* 0xe5   */ false, /* 0xe6   */ false,
  /* 0xe7   */ false, /* 0xe8   */ false, /* 0xe9   */ false,
  /* 0xea   */ false, /* 0xeb   */ false, /* 0xec   */ false,
  /* 0xed   */ false, /* 0xee   */ false, /* 0xef   */ false,
  /* 0xf0   */ false, /* 0xf1   */ false, /* 0xf2   */ false,
  /* 0xf3   */ false, /* 0xf4   */ false, /* 0xf5   */ false,
  /* 0xf6   */ false, /* 0xf7   */ false, /* 0xf8   */ false,
  /* 0xf9   */ false, /* 0xfa   */ false, /* 0xfb   */ false,
  /* 0xfc   */ false, /* 0xfd   */ false, /* 0xfe   */ false};

/*
 * ext_key_input_stream_t::skip_line
 */
int ext_key_input_stream_t::skip_line(void)
{
    int c;
    do {
        c = input_file->get();
    } while (!is_newline_char(c) && c != EOF);
    return c;
}

/*
 * ext_key_input_stream_t::read_char
 */
int ext_key_input_stream_t::read_char(void)
{
    int lookahead_1 = input_file->get();
    count++;
    if (is_scanning_value && is_newline_char(lookahead_1)) {
        while (true) {
            int lookahead_2 = input_file->peek();
            if (lookahead_1 == '\r' && lookahead_2 == '\n') {
                lookahead_1 = input_file->get();
                count++;
                lookahead_2 = input_file->peek();
            }
            if (lookahead_2 == ' ') {
                input_file->get();
                count++;
                lookahead_2 = input_file->peek();
                if (lookahead_2 == '#') {
                    lookahead_1 = skip_line();
                    continue;
                }
                if (is_newline_char(lookahead_2)) {
                    lookahead_1 = lookahead_2;
                    continue;
                }
                lookahead_1 = input_file->get();
                count++;
            }
            return lookahead_1;
        }
    }
    return lookahead_1;
}

/*
 * ext_key_input_stream_t::scan_name
 * A name must start with a letter and end with a colon. Valid characters are all ASCII
 * letters, numbers and the hyphen. Comparison of names is done case insensitively. Names may
 * be used several times to represent an array of values. Note that the name “Key” is special
 * in that it is madandory must occur only once.
 */

std::string ext_key_input_stream_t::scan_name(int c)
{
    std::string name;
    if (!is_alpha(c)) {
        ext_key_error(sexp_exception_t::error,
                      isprint(next_char) ?
                        "unexpected character '%c' (0x%x) found starting a name field" :
                        "unexpected character '0x%x' found starting a name field",
                      c,
                      c,
                      count);
    } else {
        name += (char) c;
        c = read_char();
        while (c != ':') {
            if (c == EOF) {
                ext_key_error(sexp_exception_t::error, "unexpected end of file", 0, 0, count);
            }
            if (is_newline_char(c)) {
                ext_key_error(sexp_exception_t::error, "unexpected end of line", 0, 0, count);
            }
            if (!is_namechar(c)) {
                ext_key_error(sexp_exception_t::error,
                              isprint(next_char) ?
                                "unexpected character '%c' (0x%x) found in a name field" :
                                "unexpected character '0x%x' found in a name field",
                              c,
                              c,
                              count);
            }
            name += (int) c;
            c = read_char();
        }
    }
    return name;
}

/*
 * ext_key_input_stream_t::scan_value
 * Values are UTF-8 encoded strings. Values can be wrapped at any point, and continued in
 * the next line indicated by leading whitespace. A continuation line with one leading space
 * does not introduce a blank so that the lines can be effectively concatenated. A blank
 * line as part of a continuation line encodes a newline.
 */
std::string ext_key_input_stream_t::scan_value(void)
{
    std::string value;
    int         c;
    do {
        c = read_char();
    } while (is_white_space(c));
    while (c != EOF && !is_newline_char(c)) {
        value += c;
        c = read_char();
    }
    return value;
}

/*
 * ext_key_input_stream_t::scan
 * GnuPG 2.3+ uses a new format to store private keys that is both more flexible and easier to
 * read and edit by human beings. The new format stores name, value-pairs using the common mail
 * and http header convention.
 */
void ext_key_input_stream_t::scan(extended_private_key_t &res)
{
    set_byte_size(8);
    int c = read_char();
    if (c == '(') {
        set_next_char(c);
        res.key.parse(this);
        has_key = true;
    } else {
        while (c != EOF) {
            // Comparison of names is done case insensitively
            std::string name = scan_name(c);
            // The name “Key” is special in that it is mandatory and must occur only once.
            // The associated value holds the actual S-expression with the cryptographic key.
            // The S-expression is formatted using the ‘Advanced Format’
            // (GCRYSEXP_FMT_ADVANCED) that avoids non-printable characters so that the file
            // can be easily inspected and edited.
            is_scanning_value = true;
            if (extended_private_key_t::iequals(name, "key")) {
                if (has_key) {
                    ext_key_error(sexp_exception_t::error,
                                  "'key' field must occur only once",
                                  0,
                                  0,
                                  count);
                }
                do {
                    c = read_char();
                } while (is_white_space(c));
                set_next_char(c);
                res.key.parse(this);
                has_key = true;
            } else {
                std::string value = scan_value();
                res.fields.insert(std::pair<std::string, std::string>{name, value});
            }
            c = read_char();
            is_scanning_value = false;
        }
    }
    if (!has_key) {
        ext_key_error(sexp_exception_t::error, "missing mandatory 'key' field", 0, 0, count);
    }
}

/*
 * extended_private_key_t::parse
 */
void extended_private_key_t::parse(ext_key_input_stream_t &is)
{
    is.scan(*this);
}

} // namespace ext_key_format