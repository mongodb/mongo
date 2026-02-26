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

#include "sexp-tests.h"
#include "sexpp/ext-key-format.h"

using namespace sexp;
using namespace ext_key_format;

using ::testing::UnitTest;

namespace {
class G23ExceptionTests : public testing::Test {
  protected:
    static void do_scan_ex(const char *fn, const char *msg)
    {
        std::ifstream ifs(sexp_samples_folder + "/compat/g23/" + fn, std::ifstream::binary);
        EXPECT_FALSE(ifs.fail());
        if (!ifs.fail()) {
            try {
                ext_key_input_stream_t is(&ifs);
                extended_private_key_t extended_key;
                is.scan(extended_key);
                FAIL() << "sexp::sexp_exception_t expected but has not been thrown";
            } catch (sexp::sexp_exception_t &e) {
                EXPECT_STREQ(e.what(), msg);
            }
        }
    }
};

// Malformed extended key format, line break inside name
TEST_F(G23ExceptionTests, G23MalformedNameBreak)
{
    do_scan_ex("malformed_name_break.key",
               "EXTENDED KEY FORMAT ERROR: unexpected end of line at position 5");
}

// Malformed extended key format, eof break inside name
TEST_F(G23ExceptionTests, G23MalformedNameEof)
{
    do_scan_ex("malformed_name_eof.key",
               "EXTENDED KEY FORMAT ERROR: unexpected end of file at position 2800");
}

// Malformed extended key format, invalid character name
TEST_F(G23ExceptionTests, G23MalformedInvalidNameChar)
{
    do_scan_ex(
      "malformed_invalid_name_char.key",
      "EXTENDED KEY FORMAT ERROR: unexpected character '@' (0x40) found in a name field "
      "at position 28");
}

// Malformed extended key format, invalid character name
TEST_F(G23ExceptionTests, G23MalformedInvalidNameFirstChar)
{
    do_scan_ex(
      "malformed_invalid_name_first_char.key",
      "EXTENDED KEY FORMAT ERROR: unexpected character '1' (0x31) found starting a name field "
      "at position 21");
}

// Malformed extended key format, no key field
TEST_F(G23ExceptionTests, G23MalformedNoKey)
{
    do_scan_ex("malformed_no_key.key",
               "EXTENDED KEY FORMAT ERROR: missing mandatory 'key' field at position 2819");
}

// Malformed extended key format, two key fields
TEST_F(G23ExceptionTests, G23MalformedTwoKeys)
{
    do_scan_ex("malformed_two_keys.key",
               "EXTENDED KEY FORMAT ERROR: 'key' field must occur only once at position 2822");
}

TEST_F(G23ExceptionTests, G23Warning)
{
    testing::internal::CaptureStdout();
    sexp::sexp_exception_t::set_interactive(true);
    ext_key_error(sexp_exception_t::warning, "Test warning", 0, 0, 200);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "\n*** EXTENDED KEY FORMAT WARNING: Test warning at position 200 ***\n");
    sexp::sexp_exception_t::set_interactive(false);
}

} // namespace
