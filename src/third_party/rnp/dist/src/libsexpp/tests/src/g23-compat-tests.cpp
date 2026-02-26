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
class G23CompatTests : public testing::Test {
  protected:
    static void scan_and_check_correct(const char *fn)
    {
        std::ifstream ifs(sexp_samples_folder + "/compat/g23/" + fn, std::ifstream::binary);
        bool          r = ifs.fail();
        EXPECT_FALSE(r);

        if (!ifs.fail()) {
            ext_key_input_stream_t is(&ifs);
            extended_private_key_t extended_key;
            is.scan(extended_key);
            EXPECT_EQ(extended_key.fields.size(), 2);
            EXPECT_EQ(extended_key.fields.count("Created"), 1);
            EXPECT_EQ(extended_key.fields.count("creaTed"), 1);
            EXPECT_EQ(extended_key.fields.count("something"), 0);

            auto search = extended_key.fields.find("Created");
            if (search != extended_key.fields.end()) {
                EXPECT_EQ(search->second, "20221130T160847");
            }
        }
    }
};

TEST_F(G23CompatTests, G10Test)
{
    std::ifstream ifs(sexp_samples_folder + "/compat/g10/canonical.key",
                      std::ifstream::binary);
    bool          r = ifs.fail();
    EXPECT_FALSE(r);

    if (!ifs.fail()) {
        ext_key_input_stream_t is(&ifs);
        extended_private_key_t extended_key;
        is.scan(extended_key);
        EXPECT_EQ(extended_key.fields.size(), 0);
    }
}

// Correct extended key format
TEST_F(G23CompatTests, G23Correct)
{
    scan_and_check_correct("correct.key");
}

// Correct extended key format, no terminating end of line
TEST_F(G23CompatTests, G23CorrectNoEol)
{
    scan_and_check_correct("correct_no_eol.key");
}

// Correct extended key format, with a comment
TEST_F(G23CompatTests, G23CorrectWithComment)
{
    scan_and_check_correct("correct_with_comment.key");
}

// Correct extended key format, with an empty line (which is comment a well)
TEST_F(G23CompatTests, G23CorrectWithTwoEmptyLines)
{
    scan_and_check_correct("correct_with_two_empty_lines.key");
}

// Correct extended key format, with two empty linea
TEST_F(G23CompatTests, G23CorrectWithEmptyLine)
{
    scan_and_check_correct("correct_with_empty_line.key");
}

// Correct extended key format, witg windows line endings
TEST_F(G23CompatTests, G23CorrectWithWindowsEol)
{
    scan_and_check_correct("correct_with_windows_eol.key");
}

// Correct extended key format, with a comment at the end of file
TEST_F(G23CompatTests, G23CorrectWithCommentAtEof)
{
    scan_and_check_correct("correct_with_comment_at_eof.key");
}

// Correct extended key format, with multiple fields of the same name
TEST_F(G23CompatTests, G23CorrectWithMultFields)
{
    std::ifstream ifs(sexp_samples_folder + "/compat/g23/correct_mult_fields.key",
                      std::ifstream::binary);
    bool          r = ifs.fail();
    EXPECT_FALSE(r);

    if (!ifs.fail()) {
        ext_key_input_stream_t is(&ifs);
        extended_private_key_t extended_key;
        extended_key.parse(is);
        EXPECT_EQ(extended_key.fields.size(), 4);
        EXPECT_EQ(extended_key.fields.count("Created"), 1);
        EXPECT_EQ(extended_key.fields.count("Description"), 3);
        EXPECT_EQ(extended_key.fields.count("something"), 0);

        auto search = extended_key.fields.find("Description");
        if (search != extended_key.fields.end()) {
            EXPECT_EQ(search->second, "RSA/RSA");
        }
    }
}

} // namespace
