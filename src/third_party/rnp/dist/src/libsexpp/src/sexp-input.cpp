/**
 *
 * Copyright 2021-2025 Ribose Inc. (https://www.ribose.com)
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
 * SEXP implementation code sexp-input.c
 * Ron Rivest
 * 7/21/1997
 */

#include "sexpp/sexp.h"

namespace sexp {

/*
 * sexp_input_stream_t::sexp_input_stream_t
 * Creates and initializes new sexp_input_stream_t object.
 */

sexp_input_stream_t::sexp_input_stream_t(std::istream *i, size_t m_depth)
{
    set_input(i, m_depth);
}

/*
 * sexp_input_stream_t::set_input(std::istream *i, size_t m_depth)
 */

sexp_input_stream_t *sexp_input_stream_t::set_input(std::istream *i, size_t m_depth)
{
    input_file = i;
    byte_size = 8;
    next_char = ' ';
    bits = 0;
    n_bits = 0;
    count = -1;
    reset_depth(m_depth);
    return this;
}

/*
 * sexp_input_stream_t::set_byte_size(newByteSize)
 */
sexp_input_stream_t *sexp_input_stream_t::set_byte_size(uint32_t newByteSize)
{
    byte_size = newByteSize;
    n_bits = 0;
    bits = 0;
    return this;
}

int sexp_input_stream_t::read_char(void)
{
    count++;
    return input_file->get();
}

/*
 * sexp_input_stream_t::get_char()
 * This is one possible character input routine for an input stream.
 * (This version uses the standard input stream.)
 * get_char places next 8-bit character into is->next_char.
 * It also updates the count of number of 8-bit characters read.
 * The value EOF is obtained when no more input is available.
 * This code handles 4-bit/6-bit/8-bit channels.
 */
sexp_input_stream_t *sexp_input_stream_t::get_char(void)
{
    int c;
    if (next_char == EOF) {
        byte_size = 8;
        return this;
    }

    while (true) {
        c = next_char = read_char();
        if (c == EOF)
            return this;
        if ((byte_size == 6 && (c == '|' || c == '}')) || (byte_size == 4 && (c == '#'))) {
            // end of region reached; return terminating character, after checking for
            // unused bits
            if (n_bits > 0 && (((1 << n_bits) - 1) & bits) != 0) {
                sexp_error(sexp_exception_t::warning,
                           "%zu-bit region ended with %zu unused bits left-over",
                           byte_size,
                           n_bits,
                           count);
            }
            return set_byte_size(8);
        } else if (byte_size != 8 && is_white_space(c))
            ; /* ignore white space in hex and base64 regions */
        else if (byte_size == 6 && c == '=')
            ; /* ignore equals signs in base64 regions */
        else if (byte_size == 8) {
            return this;
        } else if (byte_size < 8) {
            bits = bits << byte_size;
            n_bits += byte_size;
            if (byte_size == 6 && is_base64_digit(c))
                bits = bits | base64value(c);
            else if (byte_size == 4 && is_hex_digit(c))
                bits = bits | hexvalue(c);
            else {
                sexp_error(sexp_exception_t::error,
                           "character '%c' found in %zu-bit coding region",
                           next_char,
                           byte_size,
                           count);
            }
            if (n_bits >= 8) {
                next_char = (bits >> (n_bits - 8)) & 0xFF;
                n_bits -= 8;
                return this;
            }
        }
    }
}

/*
 * sexp_input_stream_t::skip_white_space
 * Skip over any white space on the given sexp_input_stream_t.
 */
sexp_input_stream_t *sexp_input_stream_t::skip_white_space(void)
{
    while (is_white_space(next_char))
        get_char();
    return this;
}

/*
 * sexp_input_stream_t::skip_char(c)
 * Skip the following input character on input stream is, if it is
 * equal to the character c.  If it is not equal, then an error occurs.
 */
sexp_input_stream_t *sexp_input_stream_t::skip_char(int c)
{
    if (next_char != c)
        sexp_error(sexp_exception_t::error,
                   "character '%c' found where '%c' was expected",
                   next_char,
                   c,
                   count);
    return get_char();
}

/*
 * sexp_input_stream_t::scan_token(ss)
 * scan one or more characters into simple string ss as a token.
 */
void sexp_input_stream_t::scan_token(sexp_simple_string_t &ss)
{
    skip_white_space();
    while (is_token_char(next_char)) {
        ss.append(next_char);
        get_char();
    }
}

/*
 * sexp_input_stream_t::scan_to_eof(void)
 * scan one or more characters (until EOF reached)
 * return an object that is just that string
 */
std::shared_ptr<sexp_object_t> sexp_input_stream_t::scan_to_eof(void)
{
    sexp_simple_string_t ss;
    skip_white_space();
    while (next_char != EOF) {
        ss.append(next_char);
        get_char();
    }
    auto s = std::make_shared<sexp_string_t>();
    s->set_string(ss);
    return s;
}

/*
 * scan_decimal_string(is)
 * returns long integer that is value of decimal number
 */
uint32_t sexp_input_stream_t::scan_decimal_string(void)
{
    uint32_t value = 0;
    uint32_t i = 0;
    while (is_dec_digit(next_char)) {
        value = value * 10 + decvalue(next_char);
        get_char();
        if (i++ > 8)
            sexp_error(sexp_exception_t::error, "Decimal number is too long", count);
    }
    return value;
}

/*
 * sexp_input_stream_t::scan_verbatim_string(is,ss,length)
 * Reads verbatim string of given length into simple string ss.
 */
void sexp_input_stream_t::scan_verbatim_string(sexp_simple_string_t &ss, uint32_t length)
{
    skip_white_space()->skip_char(':');

    // Some length is specified always, this is ensured by the caller's logic
    assert(length != std::numeric_limits<uint32_t>::max());
    // We should not handle too large strings
    if (length > 1024 * 1024) {
        sexp_error(sexp_exception_t::error, "Verbatim string is too long: %zu", length, count);
    }
    for (uint32_t i = 0; i < length; i++) {
        if (next_char == EOF) {
            sexp_error(
              sexp_exception_t::error, "EOF while reading verbatim string", count);
        }
        ss.append(next_char);
        get_char();
    }
}

/*
 * sexp_input_stream_t::scan_quoted_string(ss,length)
 * Reads quoted string of given length into simple string ss.
 * Handles ordinary C escapes.
 * If of indefinite length, length is std::numeric_limits<uint32_t>::max().
 */
void sexp_input_stream_t::scan_quoted_string(sexp_simple_string_t &ss, uint32_t length)
{
    skip_char('"');
    while (ss.length() <= length) {
        if (next_char == '\"') {
            if (length == std::numeric_limits<uint32_t>::max() || (ss.length() == length)) {
                skip_char('\"');
                return;
            } else
                sexp_error(sexp_exception_t::error,
                           "Declared length was %zu, but quoted string ended too early",
                           length,
                           count);
        } else if (next_char == '\\') /* handle escape sequence */
        {
            get_char();
            switch (next_char) {
            case 'b':
                ss.append('\b');
                break;
            case 't':
                ss.append('\t');
                break;
            case 'v':
                ss.append('\v');
                break;
            case 'n':
                ss.append('\n');
                break;
            case 'f':
                ss.append('\f');
                break;
            case 'r':
                ss.append('\r');
                break;
            case '\"':
                ss.append('\"');
                break;
            case '\'':
                ss.append('\'');
                break;
            case '\\':
                ss.append('\\');
                break;
            case 'x': /* hexadecimal number */
            {
                int j;
                uint8_t val = 0; // Handle 2 hex digits, no overflow is possible
                get_char();
                for (j = 0; j < 2; j++) {
                    if (is_hex_digit(next_char)) {
                        val = ((val << 4) | hexvalue(next_char));
                        if (j < 1) {
                            get_char();
                        }
                    } else
                        sexp_error(sexp_exception_t::error,
                                   "Hex character \x5cx%x... too short",
                                   val,
                                   count);
                }
                ss.append(val);
            } break;
            case '\n':      /* ignore backslash line feed */
                get_char(); /* also ignore following carriage-return if present */
                if (next_char != '\r')
                    continue;
                break;
            case '\r':      /* ignore backslash carriage-return */
                get_char(); /* also ignore following linefeed if present */
                if (next_char != '\n')
                    continue;
                break;
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7': { /* octal number */
                int j;
                uint32_t val = 0; // Handle 3 octal digits with possible overflow
                for (j = 0; j < 3; j++) {
                    if (next_char >= '0' && next_char <= '7') {
                        val = ((val << 3) | (next_char - '0'));
                        if (j < 2)
                            get_char();
                    } else
                        sexp_error(sexp_exception_t::error,
                                   "Octal character \\%o... too short",
                                   val,
                                   count);
                }
                if (val > 255)
                    sexp_error(sexp_exception_t::error, "Octal character \\%o... too big", val, count);
                ss.append(val);
            } break;
            default:
                sexp_error(sexp_exception_t::error,
                           "Unknown escape sequence \\%c", next_char, count);
            }
        } /* end of handling escape sequence */
        else if (next_char == EOF) {
            sexp_error(sexp_exception_t::error, "unexpected end of file", count);
        } else {
            ss.append(next_char);
        }
        get_char();
    } /* end of main while loop */
}

/*
 * scan_hexadecimal_string(ss,length)
 * Reads hexadecimal string into simple string ss.
 * String is of given length result, or length = std::numeric_limits<uint32_t>::max()
 * if indefinite length.
 */
void sexp_input_stream_t::scan_hexadecimal_string(sexp_simple_string_t &ss, uint32_t length)
{
    set_byte_size(4)->skip_char('#');
    while (next_char != EOF && (next_char != '#' || get_byte_size() == 4)) {
        ss.append(next_char);
        get_char();
    }
    skip_char('#');
    if (ss.length() != length && length != std::numeric_limits<uint32_t>::max())
        sexp_error(sexp_exception_t::warning,
                   "Hex string has length %zu different than declared length %zu",
                   ss.length(),
                   length,
                   count);
}

/*
 * sexp_input_stream_t::scan_base64_string(ss,length)
 * Reads base64 string into simple string ss.
 * String is of given length result, or length = std::numeric_limits<uint32_t>::max()
 * if indefinite length.
 */
void sexp_input_stream_t::scan_base64_string(sexp_simple_string_t &ss, uint32_t length)
{
    set_byte_size(6)->skip_char('|');
    while (next_char != EOF && (next_char != '|' || get_byte_size() == 6)) {
        ss.append(next_char);
        get_char();
    }
    skip_char('|');
    if (ss.length() != length && length != std::numeric_limits<uint32_t>::max())
        sexp_error(sexp_exception_t::warning,
                   "Base64 string has length %zu different than declared length %zu",
                   ss.length(),
                   length,
                   count);
}

/*
 * sexp_input_stream_t::scan_simple_string(void)
 * Reads and returns a simple string from the input stream.
 * Determines type of simple string from the initial character, and
 * dispatches to appropriate routine based on that.
 */
sexp_simple_string_t sexp_input_stream_t::scan_simple_string(void)
{
    uint32_t             length;
    sexp_simple_string_t ss;
    skip_white_space();
    /* Note that it is important in the following code to test for token-ness
     * before checking the other cases, so that a token may begin with ":",
     * which would otherwise be treated as a verbatim string missing a length.
     */
    if (is_token_char(next_char) && !is_dec_digit(next_char)) {
        scan_token(ss);
    } else {
        length = is_dec_digit(next_char) ? scan_decimal_string() :
                                           std::numeric_limits<uint32_t>::max();

        switch (next_char) {
        case '\"':
            scan_quoted_string(ss, length);
            break;
        case '#':
            scan_hexadecimal_string(ss, length);
            break;
        case '|':
            scan_base64_string(ss, length);
            break;
        case ':':
            // ':' is 'tokenchar', so some length shall be defined
            scan_verbatim_string(ss, length);
            break;
        default: {
            const char *const msg = (next_char == EOF) ? "unexpected end of file" :
                                    isprint(next_char) ? "illegal character '%c' (0x%x)" :
                                                         "illegal character 0x%x";
            sexp_error(sexp_exception_t::error, msg, next_char, next_char, count);
        }
        }
    }

    if (ss.length() == 0)
        sexp_error(sexp_exception_t::warning, "Simple string has zero length", count);
    return ss;
}

/*
 * sexp_input_stream_t::scan_string(void)
 * Reads and returns a string [presentationhint]string from input stream.
 */
std::shared_ptr<sexp_string_t> sexp_input_stream_t::scan_string(void)
{
    auto s = std::make_shared<sexp_string_t>();
    ;
    s->parse(this);
    return s;
}

/*
 * sexp_input_stream_t::scan_list(void)
 * Read and return a sexp_list_t from the input stream.
 */
std::shared_ptr<sexp_list_t> sexp_input_stream_t::scan_list(void)
{
    auto list = std::make_shared<sexp_list_t>();
    list->parse(this);
    return list;
}

/*
 * sexp_input_stream_t::scan_object(void)
 * Reads and returns a sexp_object_t from the given input stream.
 */
std::shared_ptr<sexp_object_t> sexp_input_stream_t::scan_object(void)
{
    std::shared_ptr<sexp_object_t> object;
    skip_white_space();
    if (next_char == '{' && byte_size != 6) {
        set_byte_size(6)->skip_char('{');
        object = scan_object();
        skip_char('}');
    } else {
        if (next_char == '(')
            object = scan_list();
        else
            object = scan_string();
    }
    return object;
}

/*
 * sexp_input_stream_t::open_list(void)
 */
sexp_input_stream_t *sexp_input_stream_t::open_list(void)
{
    skip_char('(');
    // gcc 4.8.5 generates wrong code in case of chaining like
    //           skip_char('(')->increase_depth(count)
    increase_depth(count);
    return this;
}
/*
 * sexp_input_stream_t::close_list(void)
 */
sexp_input_stream_t *sexp_input_stream_t::close_list(void)
{
    skip_char(')');
    decrease_depth();
    return this;
}

} // namespace sexp
