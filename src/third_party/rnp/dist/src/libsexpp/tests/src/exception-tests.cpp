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
 */

#include "sexp-tests.h"

using namespace sexp;

namespace {
class ExceptionTests : public testing::Test {
  protected:
    static void do_scan_with_exception(const char *str_in, const char *msg)
    {
        try {
            std::istringstream  iss(str_in);
            sexp_input_stream_t is(&iss);
            is.set_byte_size(8)->get_char()->scan_object();
            FAIL() << "sexp::sexp_exception_t expected but has not been thrown";
        } catch (sexp::sexp_exception_t &e) {
            EXPECT_STREQ(e.what(), msg);
        }
    }
};

TEST_F(ExceptionTests, UnexpectedEof)
{
    do_scan_with_exception("(4:This2:is1:a4:test",
                           "SEXP ERROR: unexpected end of file at position 20");
}

TEST_F(ExceptionTests, UnexpectedCharacter4bit)
{
    do_scan_with_exception(
      "(4:This2:is1:a4:test #)",
      "SEXP ERROR: character ')' found in 4-bit coding region at position 22");
}

TEST_F(ExceptionTests, IllegalCharacter)
{
    do_scan_with_exception("(This is a test ?)",
                           "SEXP ERROR: illegal character '?' (0x3f) at position 16");
}

TEST_F(ExceptionTests, UnexpectedEofAfterQuote)
{
    do_scan_with_exception("(\")\n", "SEXP ERROR: unexpected end of file at position 4");
}

TEST_F(ExceptionTests, IllegalCharacterBase64)
{
    do_scan_with_exception("(Test {KDQ6VGhpczI6aXMxOmE0OnRlc3Qq})",
                           "SEXP ERROR: illegal character '}' (0x7d) at position 35");
}

TEST_F(ExceptionTests, InvalidHex)
{
    do_scan_with_exception("(\"\\x1U\")",
                           "SEXP ERROR: Hex character \\x1... too short at position 5");
}

TEST_F(ExceptionTests, InvalidOctal)
{
    do_scan_with_exception("(\"\\12U\")",
                           "SEXP ERROR: Octal character \\12... too short at position 5");
}

TEST_F(ExceptionTests, TooBigOctal)
{
    do_scan_with_exception("(\"\\666U\")",
                           "SEXP ERROR: Octal character \\666... too big at position 5");
}

TEST_F(ExceptionTests, InvalidEscape)
{
    do_scan_with_exception("(\"\\?\")",
                           "SEXP ERROR: Unknown escape sequence \\? at position 3");
}

TEST_F(ExceptionTests, StringTooShortQuoted)
{
    do_scan_with_exception(
      "(4\"ABC\")",
      "SEXP ERROR: Declared length was 4, but quoted string ended too early at position 6");
}

TEST_F(ExceptionTests, StringTooShortBase64)
{
    sexp::sexp_exception_t::set_verbosity(sexp::sexp_exception_t::warning);
    do_scan_with_exception("(8|NDpBQkNE|)",
                           "SEXP WARNING: Base64 string has length 6 different than declared "
                           "length 8 at position 12");
    sexp::sexp_exception_t::set_verbosity(sexp::sexp_exception_t::error);
}

TEST_F(ExceptionTests, StringTooShortHex)
{
    sexp::sexp_exception_t::set_verbosity(sexp::sexp_exception_t::warning);
    do_scan_with_exception(
      "(8#AAABFCAD#)",
      "SEXP WARNING: Hex string has length 4 different than declared length 8 at position 12");
    sexp::sexp_exception_t::set_verbosity(sexp::sexp_exception_t::error);
}

TEST_F(ExceptionTests, StringBadLength)
{
    do_scan_with_exception("(1A:AAABFCAD)",
                           "SEXP ERROR: illegal character 'A' (0x41) at position 2");
}

TEST_F(ExceptionTests, StringTooLongTruncated)
{
    do_scan_with_exception(
      "(982582599:", "SEXP ERROR: Verbatim string is too long: 982582599 at position 11");}

TEST_F(ExceptionTests, StringTruncated)
{
    do_scan_with_exception("(1024:",
                           "SEXP ERROR: EOF while reading verbatim string at position 6");
}

TEST_F(ExceptionTests, DecimalTooLong)
{
    do_scan_with_exception("(1234567890:AAABFCAD)",
                           "SEXP ERROR: Decimal number is too long at position 11");
}

TEST_F(ExceptionTests, Base64CurlyBracket)
{
    // "ey..." in base64 encoding translates to "{..."
    do_scan_with_exception("({ey})", "SEXP ERROR: illegal character '{' (0x7b) at position 3");
}

TEST_F(ExceptionTests, UnusedBits)
{
    sexp::sexp_exception_t::set_verbosity(sexp::sexp_exception_t::warning);
    do_scan_with_exception(
      "(Test |AABBCCDD11|)",
      "SEXP WARNING: 6-bit region ended with 4 unused bits left-over at position 17");
    sexp::sexp_exception_t::set_verbosity(sexp::sexp_exception_t::error);
}

TEST_F(ExceptionTests, NotAListWhenExpected)
{
    try {
        std::istringstream iss(
          "|d738/4ghP9rFZ0gAIYZ5q9y6iskDJwASi5rEQpEQq8ZyMZeIZzIAR2I5iGE=|");
        sexp_input_stream_t is(&iss);

        sexp_list_t a_list;
        a_list.parse(is.set_byte_size(8)->get_char());
        FAIL() << "sexp::sexp_exception_t expected but has not been thrown";
    } catch (sexp::sexp_exception_t &e) {
        EXPECT_STREQ(e.what(),
                     "SEXP ERROR: character '|' found where '(' was expected at position 0");
    }
}

TEST_F(ExceptionTests, InvalidByteSizeAndMode)
{
    try {
        std::istringstream             iss("(3:a\011c)");
        sexp_input_stream_t            is(&iss);
        std::shared_ptr<sexp_object_t> obj = is.set_byte_size(8)->get_char()->scan_object();

        std::ostringstream   oss(std::ios_base::binary);
        sexp_output_stream_t os(&oss);
        os.change_output_byte_size(4, sexp_output_stream_t::advanced)->print_advanced(obj);
        FAIL() << "sexp::sexp_exception_t expected but has not been thrown";
    } catch (sexp::sexp_exception_t &e) {
        EXPECT_STREQ(
          e.what(),
          "SEXP ERROR: Can't print in advanced mode with restricted output character set");
    }
}

TEST_F(ExceptionTests, SexpWarning)
{
    testing::internal::CaptureStdout();
    sexp::sexp_exception_t::set_interactive(true);
    sexp_error(sexp_exception_t::warning, "Test warning", 0, 0, 200);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "\n*** SEXP WARNING: Test warning at position 200 ***\n");
    sexp::sexp_exception_t::set_interactive(false);
}

static void do_parse_list_from_string(const char *str)
{
    std::istringstream  iss(str);
    sexp_input_stream_t is(&iss);
    sexp_list_t         lst;
    lst.parse(is.set_byte_size(8)->get_char());
}

static void do_parse_list_from_string_with_limit(const char *str, size_t m_depth)
{
    std::istringstream  iss(str);
    sexp_input_stream_t is(&iss, m_depth);
    sexp_list_t         lst;
    lst.parse(is.set_byte_size(8)->get_char());
}

TEST_F(ExceptionTests, MaxDepthParse)
{
    const char *depth_1 = "(sexp_list_1)";
    const char *depth_4 = "(sexp_list_1 (sexp_list_2 (sexp_list_3 (sexp_list_4))))";
    const char *depth_4e = "(sexp_list_1 (sexp_list_2 (sexp_list_3 ())))";
    const char *depth_5 =
      "(sexp_list_1 (sexp_list_2 (sexp_list_3 (sexp_list_4 (sexp_list_5)))))";
    const char *depth_5e = "(sexp_list_1 (sexp_list_2 (sexp_list_3 (sexp_list_4 ()))))";

    do_parse_list_from_string(depth_1);
    do_parse_list_from_string(depth_4);
    do_parse_list_from_string(depth_4e);
    do_parse_list_from_string(depth_5);
    do_parse_list_from_string(depth_5e);

    do_parse_list_from_string_with_limit(depth_1, 4);
    do_parse_list_from_string_with_limit(depth_4, 4);
    do_parse_list_from_string_with_limit(depth_4e, 4);

    try {
        do_parse_list_from_string_with_limit(depth_5, 4);
        FAIL() << "sexp::sexp_exception_t expected but has not been thrown";
    } catch (sexp::sexp_exception_t &e) {
        EXPECT_STREQ(
          e.what(),
          "SEXP ERROR: Maximum allowed SEXP list depth (4) is exceeded at position 53");
    }

    try {
        do_parse_list_from_string_with_limit(depth_5e, 4);
        FAIL() << "sexp::sexp_exception_t expected but has not been thrown";
    } catch (sexp::sexp_exception_t &e) {
        EXPECT_STREQ(
          e.what(),
          "SEXP ERROR: Maximum allowed SEXP list depth (4) is exceeded at position 53");
    }
}

static void do_print_list_from_string(const char *str, bool advanced, size_t m_depth = 0)
{
    std::istringstream  iss(str);
    sexp_input_stream_t is(&iss);
    sexp_list_t         lst;
    lst.parse(is.set_byte_size(8)->get_char());

    std::ostringstream   oss(str);
    sexp_output_stream_t os(&oss, m_depth);
    if (advanced)
        lst.print_advanced(&os);
    else
        lst.print_canonical(&os);
}

TEST_F(ExceptionTests, MaxDepthPrintAdvanced)
{
    const char *depth_1 = "(sexp_list_1)";
    const char *depth_4 = "(sexp_list_1 (sexp_list_2 (sexp_list_3 (sexp_list_4))))";
    const char *depth_4e = "(sexp_list_1 (sexp_list_2 (sexp_list_3 ())))";
    const char *depth_5 =
      "(sexp_list_1 (sexp_list_2 (sexp_list_3 (sexp_list_4 (sexp_list_5)))))";
    const char *depth_5e = "(sexp_list_1 (sexp_list_2 (sexp_list_3 (sexp_list_4 ()))))";

    do_print_list_from_string(depth_1, true);
    do_print_list_from_string(depth_4, true);
    do_print_list_from_string(depth_4e, true);
    do_print_list_from_string(depth_5, true);
    do_print_list_from_string(depth_5e, true);

    do_print_list_from_string(depth_1, true, 4);
    do_print_list_from_string(depth_4, true, 4);
    do_print_list_from_string(depth_4e, true, 4);

    try {
        do_print_list_from_string(depth_5, true, 4);
        FAIL() << "sexp::sexp_exception_t expected but has not been thrown";
    } catch (sexp::sexp_exception_t &e) {
        EXPECT_STREQ(e.what(), "SEXP ERROR: Maximum allowed SEXP list depth (4) is exceeded");
    }

    try {
        do_print_list_from_string(depth_5e, true, 4);
        FAIL() << "sexp::sexp_exception_t expected but has not been thrown";
    } catch (sexp::sexp_exception_t &e) {
        EXPECT_STREQ(e.what(), "SEXP ERROR: Maximum allowed SEXP list depth (4) is exceeded");
    }
}

TEST_F(ExceptionTests, MaxDepthPrintCanonical)
{
    const char *depth_1 = "(sexp_list_1)";
    const char *depth_4 = "(sexp_list_1 (sexp_list_2 (sexp_list_3 (sexp_list_4))))";
    const char *depth_4e = "(sexp_list_1 (sexp_list_2 (sexp_list_3 ())))";
    const char *depth_5 =
      "(sexp_list_1 (sexp_list_2 (sexp_list_3 (sexp_list_4 (sexp_list_5)))))";
    const char *depth_5e = "(sexp_list_1 (sexp_list_2 (sexp_list_3 (sexp_list_4 ()))))";

    do_print_list_from_string(depth_1, false);
    do_print_list_from_string(depth_4, false);
    do_print_list_from_string(depth_4e, false);
    do_print_list_from_string(depth_5, false);
    do_print_list_from_string(depth_5e, false);

    do_print_list_from_string(depth_1, false, 4);
    do_print_list_from_string(depth_4, false, 4);
    do_print_list_from_string(depth_4e, false, 4);

    try {
        do_print_list_from_string(depth_5, false, 4);
        FAIL() << "sexp::sexp_exception_t expected but has not been thrown";
    } catch (sexp::sexp_exception_t &e) {
        EXPECT_STREQ(e.what(), "SEXP ERROR: Maximum allowed SEXP list depth (4) is exceeded");
    }

    try {
        do_print_list_from_string(depth_5e, false, 4);
        FAIL() << "sexp::sexp_exception_t expected but has not been thrown";
    } catch (sexp::sexp_exception_t &e) {
        EXPECT_STREQ(e.what(), "SEXP ERROR: Maximum allowed SEXP list depth (4) is exceeded");
    }
}

} // namespace
