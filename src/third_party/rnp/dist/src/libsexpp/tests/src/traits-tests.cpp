/**
 *
 * Copyright 2024 Ribose Inc. (https://www.ribose.com)
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

TEST(OctetTraitsTest, Assign)
{
    octet_t a = 0x12;
    octet_t b = 0x34;
    octet_traits::assign(a, b);
    EXPECT_EQ(a, b);
}

TEST(OctetTraitsTest, Eq)
{
    octet_t a = 0x12;
    octet_t b = 0x12;
    EXPECT_TRUE(octet_traits::eq(a, b));
}

TEST(OctetTraitsTest, Lt)
{
    octet_t a = 0x12;
    octet_t b = 0x34;
    EXPECT_TRUE(octet_traits::lt(a, b));
}

TEST(OctetTraitsTest, Compare)
{
    octet_t s1[] = {0x12, 0x34, 0x56};
    octet_t s2[] = {0x12, 0x34, 0x57};
    EXPECT_LT(octet_traits::compare(s1, s2, 3), 0);
}

TEST(OctetTraitsTest, Find)
{
    octet_t s[] = {0x12, 0x34, 0x56};
    octet_t a = 0x34;
    EXPECT_EQ(octet_traits::find(s, 3, a), s + 1);
}

TEST(OctetTraitsTest, Move)
{
    octet_t s1[] = {0x12, 0x34, 0x56};
    octet_t s2[3];
    octet_traits::move(s2, s1, 3);
    EXPECT_EQ(memcmp(s1, s2, 3), 0);
}

TEST(OctetTraitsTest, Copy)
{
    octet_t s1[] = {0x12, 0x34, 0x56};
    octet_t s2[3];
    octet_traits::copy(s2, s1, 3);
    EXPECT_EQ(memcmp(s1, s2, 3), 0);
}

TEST(OctetTraitsTest, AssignMultiple)
{
    octet_t s[3];
    octet_t a = 0x12;
    octet_traits::assign(s, 3, a);
    for (int i = 0; i < 3; i++) {
        EXPECT_EQ(s[i], a);
    }
}

TEST(OctetTraitsTest, ToCharType)
{
    octet_traits::int_type a = 0x12;
    EXPECT_EQ(octet_traits::to_char_type(a), 0x12);
}

TEST(OctetTraitsTest, ToIntType)
{
    octet_t a = 0x12;
    EXPECT_EQ(octet_traits::to_int_type(a), 0x12);
}

TEST(OctetTraitsTest, EqIntType)
{
    octet_traits::int_type a = 0x12;
    octet_traits::int_type b = 0x12;
    EXPECT_TRUE(octet_traits::eq_int_type(a, b));
}

TEST(OctetTraitsTest, NotEof)
{
    octet_traits::int_type a = 0x12;
    EXPECT_EQ(octet_traits::not_eof(a), 0x12);
}
} // namespace
