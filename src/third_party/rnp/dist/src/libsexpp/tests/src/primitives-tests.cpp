/**
 *
 * Copyright 2021-2024 Ribose Inc. (https://www.ribose.com)
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
class PrimitivesTests : public testing::Test {
  protected:
    static void do_test_advanced(const char *str_in, const char *str_out = nullptr)
    {
        std::istringstream  iss(str_in);
        sexp_input_stream_t is(&iss);
        const auto          obj = is.set_byte_size(8)->get_char()->scan_object();

        std::ostringstream   oss(std::ios_base::binary);
        sexp_output_stream_t os(&oss);
        os.print_advanced(obj);
        const char *sample = str_out == nullptr ? str_in : str_out;
        EXPECT_EQ(oss.str(), sample);
    }

    static void do_test_canonical(const char *str_in, const char *str_out = nullptr)
    {
        std::istringstream  iss(str_in);
        sexp_input_stream_t is(&iss);
        const auto          obj = is.set_byte_size(8)->get_char()->scan_object();

        std::ostringstream   oss(std::ios_base::binary);
        sexp_output_stream_t os(&oss);
        os.print_canonical(obj);
        const char *sample = str_out == nullptr ? str_in : str_out;
        EXPECT_EQ(oss.str(), sample);
    }
};

TEST_F(PrimitivesTests, EmptyList)
{
    do_test_canonical("( )", "()");
    do_test_advanced("( )", "()");
}

TEST_F(PrimitivesTests, EmptyString)
{
    sexp::sexp_exception_t::set_verbosity(sexp::sexp_exception_t::error);
    do_test_canonical("(\"\")", "(0:)");
    do_test_advanced("(\"\")", "(\"\")");
}

TEST_F(PrimitivesTests, String)
{
    sexp::sexp_exception_t::set_verbosity(sexp::sexp_exception_t::error);
    do_test_canonical("(ab)", "(2:ab)");
    do_test_advanced("(ab)", "(ab)");
}

TEST_F(PrimitivesTests, QuotedStringWithOctal)
{
    sexp::sexp_exception_t::set_verbosity(sexp::sexp_exception_t::error);
    do_test_canonical("\"ab\\015\"", "3:ab\r");
    do_test_advanced("\"ab\\015\"", "#61620D#");
}

TEST_F(PrimitivesTests, QuotedStringWithEscape)
{
    sexp::sexp_exception_t::set_verbosity(sexp::sexp_exception_t::error);
    do_test_canonical("\"ab\\tc\"", "4:ab\tc");
    do_test_advanced("4:ab\tc", "#61620963#");
}

TEST_F(PrimitivesTests, HexString)
{
    sexp::sexp_exception_t::set_verbosity(sexp::sexp_exception_t::error);
    do_test_canonical("#616263#", "3:abc");
    do_test_advanced("#616263#", "abc");
}

TEST_F(PrimitivesTests, ListList)
{
    do_test_canonical("(string-level-1 (string-level-2) )",
                      "(14:string-level-1(14:string-level-2))");
    do_test_advanced("(string-level-1 (string-level-2) )",
                     "(string-level-1 (string-level-2))");
}

TEST_F(PrimitivesTests, Base64Ofoctet_t)
{
    do_test_canonical("|YWJj|", "3:abc");
    do_test_advanced("|YWJj|", "abc");
}

TEST_F(PrimitivesTests, Base64OfVerbatim)
{
    do_test_canonical("{MzphYmM=}", "3:abc");
    do_test_advanced("{MzphYmM=}", "abc");
}

TEST_F(PrimitivesTests, MultilineLinux)
{
    do_test_canonical("\"abcd\\\nef\"", "6:abcdef");
    do_test_advanced("\"abcd\\\nef\"", "abcdef");
}

TEST_F(PrimitivesTests, MultilineMac)
{
    do_test_canonical("\"abcd\\\ref\"", "6:abcdef");
    do_test_advanced("\"abcd\\\ref\"", "abcdef");
}

TEST_F(PrimitivesTests, MultilineWin)
{
    do_test_canonical("\"abcd\\\r\nef\"", "6:abcdef");
    do_test_advanced("\"abcd\\\r\nef\"", "abcdef");
}

TEST_F(PrimitivesTests, MultilineBsd)
{
    do_test_canonical("\"abcd\\\n\ref\"", "6:abcdef");
    do_test_advanced("\"abcd\\\n\ref\"", "abcdef");
}

TEST_F(PrimitivesTests, Wrap)
{
    const char *reallyLong = "(a (b (c ddddddddddddddd"
                             "ddddddddddddddddddddddddddddddddddddddddddddddddd"
                             "ddddddddddddddddddddddddddddddddddddddddddddddddd"
                             "ddddddddddddddddddddddddddddddddddddddddddddddddd"
                             "ddddddd)))";
    const char *stillLong = "(1:a(1:b(1:c169:ddddddddd"
                            "ddddddddddddddddddddddddddddddddddddddddddddddddd"
                            "ddddddddddddddddddddddddddddddddddddddddddddddddd"
                            "ddddddddddddddddddddddddddddddddddddddddddddddddd"
                            "ddddddddddddd)))";
    const char *broken = "(a\n"
                         " (b\n"
                         "  (c\n"
                         "   "
                         "\"ddddddddddddddddddddddddddddddddddddddddddddddddddddd"
                         "dddddddddddddddd\\\n"
                         "ddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
                         "dddddddddddddddddd\\\n"
                         "ddddddddddddddddddddddddddd\")))";
    do_test_canonical(reallyLong, stillLong);
    do_test_advanced(reallyLong, broken);
}

TEST_F(PrimitivesTests, Escapes)
{
    do_test_canonical("(\"\\b\\t\\v\\n\\f\\r\\\"\\'\\\\\")", "(9:\b\t\v\n\f\r\"'\\)");
    do_test_advanced("(\"\\b\\t\\v\\n\\f\\r\\\"\\'\\\\\")", "(|CAkLCgwNIidc|)");

    do_test_canonical("(\"\\040\\041\\042\\043\\044\")", "(5: !\"#$)");
    do_test_advanced("(\"\\065\\061\\062\\063\\064\")", "(\"51234\")");

    do_test_canonical("(\"\\x40\\x41\\x42\\x43\\x44\")", "(5:@ABCD)");
    do_test_advanced("(\"\\x65\\x61\\x62\\x63\\x64\")", "(eabcd)");
}

TEST_F(PrimitivesTests, at4rnp)
{
    const char *str_in = "(rnp_block (rnp_list1 rnp_list2))";

    std::istringstream  iss(str_in);
    sexp_input_stream_t is(&iss);

    sexp_list_t lst;
    lst.parse(is.set_byte_size(8)->get_char());

    EXPECT_EQ(lst.sexp_list_at(0), nullptr);
    EXPECT_NE(lst.sexp_list_at(1), nullptr);

    EXPECT_NE(lst.sexp_string_at(0), nullptr);
    EXPECT_EQ(lst.sexp_string_at(1), nullptr);

    const sexp_object_t *obj = lst.sexp_list_at(1);

    if (obj != nullptr) {
        EXPECT_EQ(obj->sexp_list_at(0), nullptr);
        EXPECT_EQ(obj->sexp_list_at(1), nullptr);
    }

    const sexp_string_t *sstr = lst.sexp_string_at(0);
    EXPECT_STREQ(reinterpret_cast<const char *>(sstr->get_string().c_str()), "rnp_block");
}

TEST_F(PrimitivesTests, eq4rnp)
{
    const char *str_in = "(rnp_block (rnp_list1 rnp_list2))";

    std::istringstream  iss(str_in);
    sexp_input_stream_t is(&iss);

    sexp_list_t lst;
    lst.parse(is.set_byte_size(8)->get_char());

    EXPECT_TRUE(*lst.at(0) == "rnp_block");
    EXPECT_FALSE(*lst.at(0) == "not_rnp_block");
    EXPECT_FALSE(*lst.at(1) == "rnp_block");
    EXPECT_FALSE(*lst.at(1) == "not_rnp_block");

    EXPECT_TRUE(*lst.sexp_string_at(0) == "rnp_block");
    EXPECT_FALSE(*lst.sexp_string_at(0) == "not_rnp_block");
    EXPECT_TRUE(*lst.sexp_simple_string_at(0) == "rnp_block");
    EXPECT_FALSE(*lst.sexp_simple_string_at(0) == "not_rnp_block");

    EXPECT_TRUE(*lst.sexp_list_at(1)->at(0) == "rnp_list1");
    EXPECT_TRUE(*lst.sexp_list_at(1)->sexp_string_at(1) == "rnp_list2");

    EXPECT_TRUE(lst.sexp_string_at(0) == std::string("rnp_block"));
    EXPECT_FALSE(lst.sexp_string_at(0) == std::string("not_rnp_block"));
    EXPECT_TRUE(lst.sexp_simple_string_at(0) == std::string("rnp_block"));
    EXPECT_FALSE(lst.sexp_simple_string_at(0) == std::string("not_rnp_block"));
}

TEST_F(PrimitivesTests, ne4rnp)
{
    const char *str_in = "(rnp_block (rnp_list1 rnp_list2))";

    std::istringstream  iss(str_in);
    sexp_input_stream_t is(&iss);

    sexp_list_t lst;
    lst.parse(is.set_byte_size(8)->get_char());

    EXPECT_FALSE(*lst.at(0) != "rnp_block");
    EXPECT_TRUE(*lst.at(0) != "not_rnp_block");
    EXPECT_TRUE(*lst.at(1) != "rnp_block");
    EXPECT_TRUE(*lst.at(1) != "not_rnp_block");

    EXPECT_FALSE(*lst.sexp_string_at(0) != "rnp_block");
    EXPECT_TRUE(*lst.sexp_string_at(0) != "not_rnp_block");
    EXPECT_FALSE(*lst.sexp_simple_string_at(0) != "rnp_block");
    EXPECT_TRUE(*lst.sexp_simple_string_at(0) != "not_rnp_block");

    EXPECT_FALSE(*lst.sexp_list_at(1)->at(0) != "rnp_list1");
    EXPECT_FALSE(*lst.sexp_list_at(1)->sexp_string_at(1) != "rnp_list2");

    EXPECT_FALSE(lst.sexp_string_at(0) != std::string("rnp_block"));
    EXPECT_TRUE(lst.sexp_string_at(0) != std::string("not_rnp_block"));
    EXPECT_FALSE(lst.sexp_simple_string_at(0) != std::string("rnp_block"));
    EXPECT_TRUE(lst.sexp_simple_string_at(0) != std::string("not_rnp_block"));
}

TEST_F(PrimitivesTests, u4rnp)
{
    const char *str_in1 = "(unsigned_value \"12345\")";
    const char *str_in2 = "(14:unsigned_value5:54321)";

    std::istringstream iss1(str_in1);
    std::istringstream iss2(str_in2);

    sexp_input_stream_t is(&iss1);
    sexp_list_t         lst;
    lst.parse(is.set_byte_size(8)->get_char());
    EXPECT_EQ(lst.sexp_string_at(1)->as_unsigned(), 12345);

    lst.clear();
    lst.parse(is.set_input(&iss2)->set_byte_size(8)->get_char());
    EXPECT_EQ(lst.sexp_string_at(1)->as_unsigned(), 54321);
}

TEST_F(PrimitivesTests, proInheritance)
{
    sexp_list_t lst;
    EXPECT_FALSE(lst.is_sexp_string());
    EXPECT_TRUE(lst.is_sexp_list());
    EXPECT_EQ(lst.sexp_string_view(), nullptr);
    EXPECT_EQ(lst.sexp_list_view(), &lst);
    EXPECT_EQ(lst.as_unsigned(), std::numeric_limits<uint32_t>::max());
    EXPECT_EQ(lst.sexp_list_at(0), nullptr);
    EXPECT_EQ(lst.sexp_string_at(0), nullptr);
    EXPECT_EQ(lst.sexp_simple_string_at(0), nullptr);

    sexp_string_t str;
    EXPECT_FALSE(str.is_sexp_list());
    EXPECT_TRUE(str.is_sexp_string());
    EXPECT_EQ(str.sexp_string_view(), &str);
    EXPECT_EQ(str.sexp_list_view(), nullptr);
    EXPECT_EQ(str.sexp_list_at(0), nullptr);
    EXPECT_EQ(str.sexp_string_at(0), nullptr);
    EXPECT_EQ(str.sexp_simple_string_at(0), nullptr);
}

TEST_F(PrimitivesTests, DisplayHint)
{
    do_test_canonical("(URL [URI]www.ribose.com)", "(3:URL[3:URI]14:www.ribose.com)");
    do_test_advanced("(3:URL[3:URI]14:www.ribose.com)", "(URL [URI]www.ribose.com)");
}

TEST_F(PrimitivesTests, scanToEof)
{
    const char *str_in = "ABCD";

    std::istringstream  iss(str_in);
    sexp_input_stream_t is(&iss);

    auto object = is.scan_to_eof();
    EXPECT_TRUE(object->is_sexp_string());

    is.set_byte_size(4);
    EXPECT_EQ(is.get_byte_size(), 4);

    EXPECT_EQ(is.get_char(), &is);
    EXPECT_EQ(is.get_byte_size(), 8);
}

TEST_F(PrimitivesTests, ChangeOutputByteSizeTest)
{
    std::ostringstream   oss(std::ios_base::binary);
    sexp_output_stream_t os(&oss);

    EXPECT_EQ(os.change_output_byte_size(8, sexp_output_stream_t::advanced), &os);

    try {
        os.change_output_byte_size(7, sexp_output_stream_t::advanced);
        FAIL() << "sexp::sexp_exception_t expected but has not been thrown";
    } catch (sexp::sexp_exception_t &e) {
        EXPECT_STREQ(e.what(), "SEXP ERROR: Illegal output base 7");
    }

    EXPECT_EQ(os.change_output_byte_size(4, sexp_output_stream_t::advanced), &os);

    try {
        os.change_output_byte_size(6, sexp_output_stream_t::advanced);
        FAIL() << "sexp::sexp_exception_t expected but has not been thrown";
    } catch (sexp::sexp_exception_t &e) {
        EXPECT_STREQ(e.what(), "SEXP ERROR: Illegal change of output byte size from 4 to 6");
    }
}

TEST_F(PrimitivesTests, FlushTest)
{
    std::ostringstream   oss1(std::ios_base::binary);
    std::ostringstream   oss2(std::ios_base::binary);
    sexp_output_stream_t os(&oss1);

    EXPECT_EQ(
      os.change_output_byte_size(6, sexp_output_stream_t::advanced)->print_decimal(1)->flush(),
      &os);
    EXPECT_EQ(oss1.str(), "MQ==");
    os.set_output(&oss2)
      ->change_output_byte_size(6, sexp_output_stream_t::advanced)
      ->set_max_column(2)
      ->print_decimal(2)
      ->flush();
    EXPECT_EQ(oss2.str(), "Mg\n==");
}

TEST_F(PrimitivesTests, ListWrapTest)
{
    std::istringstream  iss("(abc)");
    sexp_input_stream_t is(&iss);
    const auto          obj = is.set_byte_size(8)->get_char()->scan_object();

    std::ostringstream   oss(std::ios_base::binary);
    sexp_output_stream_t os(&oss);
    os.set_max_column(5)->print_advanced(obj);
    EXPECT_EQ(oss.str(), "(abc\n )");
}

TEST_F(PrimitivesTests, EnsureHexTest)
{
    std::istringstream  iss("(3:a\011c)");
    sexp_input_stream_t is(&iss);
    const auto          obj = is.set_byte_size(8)->get_char()->scan_object();

    std::ostringstream   oss(std::ios_base::binary);
    sexp_output_stream_t os(&oss);
    os.print_advanced(obj);
    EXPECT_EQ(oss.str(), "(#610963#)");
}

} // namespace
